;; corestorage: the block-device + filesystem-provider registry, ported from
;; servers/CoreStorage.
;;
;; The C server held two lists -- block devices and fs providers -- and tied them
;; together with one rule: each provider is offered every device via a synchronous
;; `probe` ("is this volume mine?"), first to claim wins. Block I/O was a direct
;; synchronous call through a device's read/write function pointers.
;;
;; In Lisp every actor is a context and every interaction is a message, so:
;;   - a block driver is a context answering (read lba count reply) /
;;     (write lba count data reply); it does the I/O and `send`s the result to
;;     `reply` -- the request/response shape that the context-handle-through-send
;;     fix enables.
;;   - the storage service owns the registry and MEDIATES I/O: it bounds-checks a
;;     request against the registered block_count (as storage_blockdev_read did)
;;     and forwards it to the owning driver, so the driver list stays the I/O
;;     authority.
;;   - `probe` becomes a message: storage offers a device to a provider, and the
;;     provider claims it by sending (claim <name>) back -- no re-entrant call, so
;;     a provider doing block I/O while probing cannot deadlock the service.
;;
;; Protocol (send these to the service handle):
;;   (register-blockdev  <name> <bsize> <bcount> <driver-ctx>)
;;   (register-fsprovider <name> <provider-ctx>)
;;   (claim <name>)                         ; a provider mounts device <name>
;;   (read  <name> <lba> <count> <reply>)   ; bounds-checked, forwarded to driver
;;   (write <name> <lba> <count> <data> <reply>)
;; A probed provider receives (probe <name> <bsize> <bcount> <driver> <storage>).
;; A read/write reply target receives (complete <status> [<bytes>]).

(define-module corestorage
  (export start-storage-service)
  (import driver-util)
  (define lg (make-logger 'corestorage))

  ;; device entry layout: (name bsize bcount claimed? driver)
  (define (dev-name d)    (car d))
  (define (dev-bcount d)  (caddr d))
  (define (dev-claimed d) (cadddr d))
  (define (dev-driver d)  (nth d 4))

  (define (find-dev devs name)
    (cond ((null? devs) #f)
          ((eq? (dev-name (car devs)) name) (car devs))
          (else (find-dev (cdr devs) name))))

  ;; Rebuild the device list with `name` marked claimed (lists are immutable, so
  ;; mark = replace). A claimed device is never offered to a later provider.
  (define (mark-claimed devs name)
    (map (lambda (d)
           (if (eq? (dev-name d) name)
               (list (car d) (cadr d) (caddr d) #t (nth d 4))
               d))
         devs))

  ;; Offer a device to a provider: the probe message carries everything the
  ;; provider needs to inspect the volume (drive it via driver-ctx) and the
  ;; storage handle to send (claim name) back to.
  (define (offer dev prov storage)
    (send (cadr prov)                       ; provider ctx
          (list 'probe (car dev) (cadr dev) (caddr dev) (nth dev 4) storage)))

  ;; Forward a bounds-checked block request to the owning driver; on a bad name
  ;; or out-of-range range, answer the reply target with a failure directly.
  (define (do-read devs name lba count reply)
    (let ((dev (find-dev devs name)))
      (if (or (not dev) (> (+ lba count) (dev-bcount dev)))
          (send reply (list 'complete -1 #f))
          (send (dev-driver dev) (list 'read lba count reply)))))
  (define (do-write devs name lba count data reply)
    (let ((dev (find-dev devs name)))
      (if (or (not dev) (> (+ lba count) (dev-bcount dev)))
          (send reply (list 'complete -1))
          (send (dev-driver dev) (list 'write lba count data reply)))))

  ;; The service. State is (devs provs); `serve` threads it through the loop. The
  ;; service holds no capabilities -- it only routes messages.
  (define (start-storage-service)
    (serve (list '() '())
      (lambda (state m)
        (let ((devs (car state)) (provs (cadr state)) (me (self)))
          (cond
            ((eq? (car m) 'register-blockdev)    ; (... name bsize bcount driver)
             (let ((dev (list (cadr m) (caddr m) (cadddr m) #f (nth m 4))))
               (lg "block device registered: " (cadr m))
               (for-each (lambda (p) (offer dev p me)) provs)
               (list (cons dev devs) provs)))
            ((eq? (car m) 'register-fsprovider)  ; (... name provider)
             (let ((prov (list (cadr m) (caddr m))))
               (lg "fs provider registered: " (cadr m))
               (for-each (lambda (d) (if (not (dev-claimed d)) (offer d prov me))) devs)
               (list devs (cons prov provs))))
            ((eq? (car m) 'claim)                ; (claim name)
             (lg "volume claimed: " (cadr m))
             (list (mark-claimed devs (cadr m)) provs))
            ((eq? (car m) 'read)                 ; (read name lba count reply)
             (if (ctx? (nth m 4)) (do-read devs (cadr m) (caddr m) (cadddr m) (nth m 4)))
             state)
            ((eq? (car m) 'write)                ; (write name lba count data reply)
             (if (ctx? (nth m 5)) (do-write devs (cadr m) (caddr m) (cadddr m) (nth m 4) (nth m 5)))
             state)
            (else state)))))))
