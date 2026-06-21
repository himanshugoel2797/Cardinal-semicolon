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
              (if (eq? (car m) 'complete) m (begin (set! stash (append stash (list m))) (await)))))
          (define (bulk ep mps data len dir-in?)
            (send (usb-dev-hci dev)
                  (list 'bulk (usb-dev-address dev) ep mps data len dir-in? (self)))
            (await))
          ;; One Bulk-Only command: CBW(out) -> optional data -> CSW(in). Returns
          ;; (cons csw-status data) -- data is the IN data bytevector or #f; status
          ;; 0 = good, <0 = transport failure.
          (define (bbb cmd cmdlen data datalen dir-in?)
            (set! tag (+ tag 1))
            (let ((cbw (make-bytes 31)))
              (bytes-u32-set! cbw 0 CBW-SIG)
              (bytes-u32-set! cbw 4 tag)
              (bytes-u32-set! cbw 8 datalen)
              (bytes-u8-set! cbw 12 (if dir-in? #x80 0))
              (bytes-u8-set! cbw 13 0)             ; LUN 0
              (bytes-u8-set! cbw 14 cmdlen)
              (bytes-copy-into! cbw 15 cmd cmdlen)
              (if (< (complete-n (bulk out-ep out-mps cbw 31 #f)) 31)
                  (cons -1 #f)
                  (let ((ddata (if (> datalen 0)
                                   (let ((r (bulk (if dir-in? in-ep out-ep)
                                                  (if dir-in? in-mps out-mps) data datalen dir-in?)))
                                     (if (< (complete-n r) 0) 'err (complete-data r)))
                                   #f)))
                    (if (eq? ddata 'err)
                        (cons -1 #f)
                        (let ((csw (bulk in-ep in-mps #f 13 #t)))
                          (if (or (< (complete-n csw) 13)
                                  (not (= (bytes-u32-ref (complete-data csw) 0) CSW-SIG)))
                              (cons -1 #f)
                              (cons (bytes-u8-ref (complete-data csw) 12) ddata))))))))
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

          ;; Chunked block read: assemble count blocks into one buffer, MAX-BLK at
          ;; a time. Replies (complete 0 bytes) or (complete -1 #f).
          (define (do-read lba count reply)
            (let ((out (make-bytes (* count bsize))))
              (let loop ((lba lba) (count count) (off 0))
                (if (= count 0)
                    (send reply (list 'complete 0 out))
                    (let* ((chunk (if (> count MAX-BLK) MAX-BLK count))
                           (r (scsi-read10 lba chunk)))
                      (if (or (not (= (car r) 0)) (not (cdr r)))
                          (send reply (list 'complete -1 #f))
                          (begin (bytes-copy-into! out off (cdr r) (* chunk bsize))
                                 (loop (+ lba chunk) (- count chunk) (+ off (* chunk bsize))))))))))
          (define (do-write lba count data reply)
            (let loop ((lba lba) (count count) (off 0))
              (if (= count 0)
                  (send reply (list 'complete 0))
                  (let* ((chunk (if (> count MAX-BLK) MAX-BLK count))
                         (r (scsi-write10 lba chunk (copy-bytes data off (* chunk bsize)))))
                    (if (not (= (car r) 0))
                        (send reply (list 'complete -1))
                        (loop (+ lba chunk) (- count chunk) (+ off (* chunk bsize))))))))

          ;; --- bring-up: INQUIRY (log), READ CAPACITY (size), then register ---
          (let ((inq (let ((c (make-bytes 6))) (bytes-u8-set! c 0 #x12) (bytes-u8-set! c 4 36)
                       (bbb c 6 #f 36 #t))))
            (if (= (car inq) 0) (begin (display "[usb-storage] INQUIRY ok") (newline))
                (begin (display "[usb-storage] INQUIRY failed") (newline))))
          (let ((cap (let ((c (make-bytes 10))) (bytes-u8-set! c 0 #x25) (bbb c 10 #f 8 #t))))
            (if (and (= (car cap) 0) (cdr cap))
                (begin
                  (set! bcount (+ (get-be32 (cdr cap) 0) 1))
                  (set! bsize (let ((b (get-be32 (cdr cap) 4))) (if (= b 0) 512 b)))
                  (display "[usb-storage] capacity blocks=") (display bcount)
                  (display " bsize=") (display bsize) (newline))
                (begin (display "[usb-storage] READ CAPACITY failed") (newline) (set! bcount 0))))
          (if (= bcount 0)
              (begin (display "[usb-storage] no usable capacity; not registering") (newline)
                     (let park () (recv) (park)))      ; stay alive (stop-aware via remove)
              (begin
                (send storage (list 'register-blockdev 'usb0 bsize bcount (self)))
                (display "[usb-storage] registered usb0 with corestorage") (newline)
                ;; serve block requests, draining the stash (FIFO) before fresh recv.
                (let mainloop ()
                  (let ((req (if (null? stash) (recv)
                                 (let ((m (car stash))) (set! stash (cdr stash)) m))))
                    (cond ((eq? (car req) 'read)  (do-read (cadr req) (caddr req) (cadddr req)))
                          ((eq? (car req) 'write) (do-write (cadr req) (caddr req) (cadddr req) (nth req 4)))
                          (else 'ignore))
                    (mainloop)))))))))

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
            ((= (caar ds) addr) (send (cdar ds) 'stop)
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
