;; usb-storage: USB Mass Storage (Bulk-Only Transport + SCSI), ported from
;; drivers/usb_storage. Registers with coreusb as the class handler for
;; bInterfaceClass == Mass Storage; on (probe dev) it finds the bulk IN/OUT
;; endpoints, runs INQUIRY + READ CAPACITY, and -- if the medium has capacity --
;; registers a block device with corestorage and serves its read/write requests
;; by issuing READ(10)/WRITE(10) over BBB.
;;
;; THE STASH. The block-server context interleaves two message streams on its own
;; mailbox: block requests from corestorage, and transfer completions from the
;; controller (each BBB command is three bulk transfers). A naive recv that waited
;; for a completion would swallow a block request that arrived mid-command. So the
;; transfer wait STASHES any non-completion message into a FIFO the main loop
;; drains before recv-ing fresh -- the single-context message-IO serialization the
;; cardfs port also needed. corestorage already serializes by forwarding one
;; request per message, so the stash rarely holds more than one.
(define-module usb-storage
  (export usb-storage-init)
  (import coreusb driver-util)

  (define CBW-SIG #x43425355)   ; "USBC"
  (define CSW-SIG #x53425355)   ; "USBS"
  (define MAX-BLK 4)            ; blocks per BBB command (4*512 = 2048, the data cap)

  ;; The block-server context: owns the SCSI/BBB machinery + the stash, registers
  ;; with corestorage, and answers (read lba count reply)/(write lba count data
  ;; reply). `storage` is the corestorage handle.
  (define (start-block-server dev in-ep out-ep in-mps out-mps storage)
    (spawn-restricted '()
      (lambda ()
        (let ((stash '()) (tag 0) (bsize 512) (bcount 0))
          ;; wait for a transfer completion, stashing block requests that arrive.
          (define (await)
            (let ((m (recv)))
              (if (eq? (car m) 'complete) m
                  (begin (set! stash (append stash (list m))) (await)))))
          (define (bulk ep mps data len dir-in?)
            (send (usb-dev-hci dev)
                  (list 'bulk (usb-dev-address dev) ep mps data len dir-in? (self)))
            (await))
          ;; A no-data control transfer through the SAME stash-aware await (proto's
          ;; usb-control-* drop non-completions, which would lose a stashed block
          ;; request or 'stop). Used for BOT stall recovery (CLEAR_FEATURE halt).
          (define (clear-halt ep)
            (send (usb-dev-hci dev)
                  (list 'control (usb-dev-address dev) (usb-dev-speed dev) 8
                        (make-setup USB-REQ-RECIP-ENDPOINT USB-REQ-CLEAR-FEATURE
                                    USB-FEATURE-ENDPOINT-HALT ep 0) #f 0 (self)))
            (await))
          ;; A valid 13-byte CSW echoing our tag?
          (define (csw-ok? csw mytag)
            (and (>= (complete-n csw) 13)
                 (= (bytes-u32-ref (complete-data csw) 0) CSW-SIG)
                 (= (bytes-u32-ref (complete-data csw) 4) mytag)))
          ;; Read the CSW; if the IN endpoint stalled, clear its halt and retry once
          ;; (BOT error recovery). Returns the bCSWStatus byte, or #f if unreadable.
          (define (read-csw mytag)
            (let ((csw (bulk in-ep in-mps #f 13 #t)))
              (if (csw-ok? csw mytag)
                  (bytes-u8-ref (complete-data csw) 12)
                  (begin (clear-halt in-ep)
                         (let ((c2 (bulk in-ep in-mps #f 13 #t)))
                           (if (csw-ok? c2 mytag) (bytes-u8-ref (complete-data c2) 12) #f))))))
          ;; One Bulk-Only command: CBW(out) -> optional data -> CSW(in). Returns
          ;; (cons csw-status data): data is the IN bytevector or #f; status 0=good,
          ;; 1=command failed (sense available), <0 = transport failure. A stalled
          ;; data phase clears that endpoint's halt and STILL reads the CSW, so the
          ;; device stays in sync for the next command (per the BOT recovery flow).
          (define (bbb cmd cmdlen data datalen dir-in?)
            (set! tag (+ tag 1))
            (let ((cbw (make-bytes 31)) (mytag tag) (depi (if dir-in? in-ep out-ep)))
              (bytes-u32-set! cbw 0 CBW-SIG)
              (bytes-u32-set! cbw 4 mytag)
              (bytes-u32-set! cbw 8 datalen)
              (bytes-u8-set! cbw 12 (if dir-in? #x80 0))
              (bytes-u8-set! cbw 13 0)             ; LUN 0
              (bytes-u8-set! cbw 14 cmdlen)
              (bytes-copy-into! cbw 15 cmd cmdlen)
              (if (< (complete-n (bulk out-ep out-mps cbw 31 #f)) 31)
                  (cons -1 #f)
                  (let ((ddata (if (> datalen 0)
                                   (let ((r (bulk depi (if dir-in? in-mps out-mps) data datalen dir-in?)))
                                     (if (< (complete-n r) 0)
                                         (begin (clear-halt depi) 'err)   ; stalled data
                                         (complete-data r)))
                                   #f))
                        (st (read-csw mytag)))
                    (cond ((not st) (cons -1 #f))               ; CSW unreadable
                          ((eq? ddata 'err) (cons -1 #f))       ; data stalled (device resynced)
                          (else (cons st ddata)))))))
          ;; SCSI helpers.
          (define (scsi-read10 lba count)          ; -> (status . data)
            (let ((cmd (make-bytes 10)))
              (bytes-u8-set! cmd 0 #x28)
              (put-be32! cmd 2 lba)
              (bytes-u8-set! cmd 7 (bit-extract count 8 8))
              (bytes-u8-set! cmd 8 (bitwise-and count #xFF))
              (bbb cmd 10 #f (* count bsize) #t)))
          (define (scsi-write10 lba count data)    ; -> (status . _)
            (let ((cmd (make-bytes 10)))
              (bytes-u8-set! cmd 0 #x2A)
              (put-be32! cmd 2 lba)
              (bytes-u8-set! cmd 7 (bit-extract count 8 8))
              (bytes-u8-set! cmd 8 (bitwise-and count #xFF))
              (bbb cmd 10 data (* count bsize) #f)))
          (define (scsi-tur)                       ; TEST UNIT READY -> status (0 ready)
            (let ((c (make-bytes 6))) (bytes-u8-set! c 0 #x00) (car (bbb c 6 #f 0 #f))))
          ;; REQUEST SENSE (fixed format, 18 bytes) -> the sense bytevector or #f.
          (define (scsi-request-sense)
            (let ((c (make-bytes 6)))
              (bytes-u8-set! c 0 #x03) (bytes-u8-set! c 4 18)
              (let ((r (bbb c 6 #f 18 #t))) (if (and (= (car r) 0) (cdr r)) (cdr r) #f))))
          ;; REQUEST SENSE + log key/ASC/ASCQ; returns the sense key (or -1). Issuing
          ;; it also clears a device's pending CHECK CONDITION before the retry.
          (define (log-sense where)
            (let ((s (scsi-request-sense)))
              (if (and s (>= (bytes-length s) 14))
                  (let ((key (bitwise-and (bytes-u8-ref s 2) #xF)))
                    (display "[usb-storage] ") (display where) (display ": sense key=")
                    (display key) (display " asc=") (display (bytes-u8-ref s 12))
                    (display " ascq=") (display (bytes-u8-ref s 13)) (newline) key)
                  -1)))
          ;; MODE SENSE(6), all pages -> write-protected? (#t/#f), or #f if it failed.
          ;; The WP bit is bit 7 of the device-specific byte (byte 2) of the header.
          (define (scsi-mode-sense6)
            (let ((c (make-bytes 6)))
              (bytes-u8-set! c 0 #x1A) (bytes-u8-set! c 2 #x3F) (bytes-u8-set! c 4 192)
              (let ((r (bbb c 6 #f 192 #t)))
                (if (and (= (car r) 0) (cdr r) (>= (bytes-length (cdr r)) 3))
                    (not (= 0 (bitwise-and (bytes-u8-ref (cdr r) 2) #x80)))
                    #f))))
          ;; Run a (-> (status . data)) command up to 3 times; on a command failure
          ;; (status != 0) sense the device (logging + clearing CHECK CONDITION) and
          ;; retry. Returns the last result.
          (define (cmd-retry thunk where)
            (let loop ((n 3))
              (let ((r (thunk)))
                (if (or (= (car r) 0) (<= n 1)) r
                    (begin (log-sense where) (loop (- n 1)))))))
          ;; Poll TEST UNIT READY until the medium is ready -- removable/slow devices
          ;; report not-ready (sense key 2, NOT READY) until spun up. Sense + nap
          ;; between tries. Returns #t when ready, #f if it never became ready.
          (define (wait-ready tries)
            (let loop ((n tries))
              (cond ((= (scsi-tur) 0) #t)
                    ((<= n 1) (log-sense "unit-ready") #f)
                    (else (log-sense "unit-ready") (sleep 100000000) (loop (- n 1))))))

          ;; Chunked block read: assemble count blocks into one buffer, MAX-BLK at
          ;; a time. Replies (complete 0 bytes) or (complete -1 #f).
          (define (do-read lba count reply)
            (let ((out (make-bytes (* count bsize))))
              (let loop ((lba lba) (count count) (off 0))
                (if (= count 0)
                    (send reply (list 'complete 0 out))
                    (let* ((chunk (if (> count MAX-BLK) MAX-BLK count))
                           (r (cmd-retry (lambda () (scsi-read10 lba chunk)) "read")))
                      (if (or (not (= (car r) 0)) (not (cdr r)))
                          (send reply (list 'complete -1 #f))
                          (begin (bytes-copy-into! out off (cdr r) (* chunk bsize))
                                 (loop (+ lba chunk) (- count chunk) (+ off (* chunk bsize))))))))))
          (define (do-write lba count data reply)
            (let loop ((lba lba) (count count) (off 0))
              (if (= count 0)
                  (send reply (list 'complete 0))
                  (let* ((chunk (if (> count MAX-BLK) MAX-BLK count))
                         (r (cmd-retry (lambda () (scsi-write10 lba chunk (copy-bytes data off (* chunk bsize)))) "write")))
                    (if (not (= (car r) 0))
                        (send reply (list 'complete -1))
                        (loop (+ lba chunk) (- count chunk) (+ off (* chunk bsize))))))))

          ;; --- bring-up: INQUIRY (log) -> TEST UNIT READY (wait for medium) ->
          ;; READ CAPACITY (size) -> MODE SENSE (write-protect), then register ---
          (let ((inq (let ((c (make-bytes 6))) (bytes-u8-set! c 0 #x12) (bytes-u8-set! c 4 36)
                       (bbb c 6 #f 36 #t))))
            (if (= (car inq) 0) (begin (display "[usb-storage] INQUIRY ok") (newline))
                (begin (display "[usb-storage] INQUIRY failed") (newline))))
          (if (not (wait-ready 10))
              (display "[usb-storage] unit not ready (no medium?)")   ; capacity read below will fail
              (display "[usb-storage] unit ready"))
          (newline)
          (let ((cap (cmd-retry (lambda ()
                       (let ((c (make-bytes 10))) (bytes-u8-set! c 0 #x25) (bbb c 10 #f 8 #t)))
                       "read-capacity")))
            (if (and (= (car cap) 0) (cdr cap))
                (begin
                  (set! bcount (+ (get-be32 (cdr cap) 0) 1))
                  (set! bsize (let ((b (get-be32 (cdr cap) 4))) (if (= b 0) 512 b)))
                  (display "[usb-storage] capacity blocks=") (display bcount)
                  (display " bsize=") (display bsize) (newline)
                  (let ((wp (scsi-mode-sense6)))
                    (display "[usb-storage] write-protected=") (display (if (eq? wp #t) "yes" "no"))
                    (newline)))
                (begin (display "[usb-storage] READ CAPACITY failed") (newline) (set! bcount 0))))
          (if (= bcount 0)
              (begin (display "[usb-storage] no usable capacity; not registering") (newline)
                     ;; stay alive until remove sends (stop), then exit cleanly.
                     (let park () (if (eq? (car (recv)) 'stop) 'stopped (park))))
              (begin
                (send storage (list 'register-blockdev 'usb0 bsize bcount (self)))
                (display "[usb-storage] registered usb0 with corestorage") (newline)
                ;; serve block requests, draining the stash (FIFO) before fresh recv.
                ;; A (stop) from remove exits the loop so the context ends cleanly;
                ;; every message here is a list, so (car req) is always valid.
                (let mainloop ()
                  (let ((req (if (null? stash) (recv)
                                 (let ((m (car stash))) (set! stash (cdr stash)) m))))
                    (cond ((eq? (car req) 'stop)  'stopped)   ; exit -- do NOT recurse
                          (else
                           (cond ((eq? (car req) 'read)  (do-read (cadr req) (caddr req) (cadddr req)))
                                 ((eq? (car req) 'write) (do-write (cadr req) (caddr req) (cadddr req) (nth req 4)))
                                 (else 'ignore))
                           (mainloop)))))))))))

  (define (stor-on-probe dev storage devs)
    (let ((inep (usb-find-endpoint dev USB-XFER-BULK #t))
          (outep (usb-find-endpoint dev USB-XFER-BULK #f)))
      (if (or (not inep) (not outep))
          (begin (display "[usb-storage] missing bulk endpoints; not claiming") (newline) devs)
          (begin
            (display "[usb-storage] claimed mass-storage device") (newline)
            (let ((srv (start-block-server dev (car inep) (car outep)
                                           (let ((m (cadr inep))) (if (> m 0) m 64))
                                           (let ((m (cadr outep))) (if (> m 0) m 64))
                                           storage)))
              (cons (cons (usb-dev-address dev) srv) devs))))))

  (define (stor-on-remove addr devs)
    (let loop ((ds devs) (keep '()))
      (cond ((null? ds) keep)
            ((= (caar ds) addr) (send (cdar ds) (list 'stop))   ; a list, so the server's (car req) is valid
                                (display "[usb-storage] device removed") (newline)
                                (loop (cdr ds) keep))
            (else (loop (cdr ds) (cons (car ds) keep))))))

  ;; init.clp calls (usb-storage-init usb storage).
  (define (usb-storage-init usb storage)
    (let ((ctx (serve '()
                 (lambda (devs m)
                   (cond ((eq? (car m) 'probe)  (stor-on-probe (cadr m) storage devs))
                         ((eq? (car m) 'remove) (stor-on-remove (cadr m) devs))
                         (else devs))))))
      (send usb (list 'register-class USB-CLASS-MASS-STORAGE ctx))
      ctx)))
