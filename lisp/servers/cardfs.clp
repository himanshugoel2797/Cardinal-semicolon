;; cardfs: a CoreStorage filesystem provider + flat key->object store, ported from
;; drivers/cardfs/src/main.c.
;;
;; cardfs is the simplest expression of the "objects with keys" model -- a flat
;; key -> blob map on a block device -- built to exercise the on-disk persistence
;; path end to end before the real log-structured/COW design lands. On-disk layout
;; (512-byte blocks; the system's single-device assumption, per the C):
;;   LBA 0                 : superblock (magic "CARDFS01" + bump-alloc cursors)
;;   LBA 1 .. TABLE-BLOCKS : object table (128-byte entries: id/key/data_lba/size/valid)
;;   LBA DATA-START ..     : object data blocks (bump-allocated)
;;
;; CoreStorage drives block I/O by MESSAGE, not a synchronous call: a provider
;; reads a block by sending (read name lba count self) to the storage handle and
;; receiving (complete status bytes). cardfs is a single context, so it does that
;; round-trip SYNCHRONOUSLY inside a request handler (blk-read/blk-write) -- storage
;; forwards to the driver and returns to its own loop, so there is no re-entrant
;; deadlock. But a NEW request can land in the mailbox while a block I/O is in
;; flight; the I/O wait DEFERS any such message into `stash` and the main loop
;; re-processes it, so overlapping requests are serialised through this one context.
;; This is the faithful port of the C's read_super / format / put / get; the probe
;; (auto-mount) is the CoreStorage integration point, and the format/put/get message
;; API is what the in-OS self-test (and, later, a userspace surface) drives.
;; Deliberately still absent (as in the C): free-space reclaim, tags/relations, a
;; B-tree, crash consistency -- the next design, not this slice.

(define-module cardfs
  (export start-cardfs)
  (import driver-util)

  (define CARDFS-MAGIC (list 67 65 82 68 70 83 48 49)) ; "CARDFS01"
  (define TABLE-LBA    1)
  (define TABLE-BLOCKS 8)
  (define DATA-START   (+ TABLE-LBA TABLE-BLOCKS))     ; first data LBA (9)
  (define KEY-LEN      64)
  (define ENTRY-SIZE   128)
  (define DEFAULT-BS   512)

  ;; --- superblock (LBA 0) -----------------------------------------------------
  ;; Field offsets: magic@0(8) block_size@8(u32) table_blocks@12(u32) table_lba@16
  ;; data_start@24 next_free@32 next_obj_id@40 obj_count@48 (u64 after block_size).
  (define (magic-ok? sb)
    (and (>= (bytes-length sb) 8)
         (let loop ((i 0) (m CARDFS-MAGIC))
           (cond ((null? m) #t)
                 ((= (bytes-u8-ref sb i) (car m)) (loop (+ i 1) (cdr m)))
                 (else #f)))))
  (define (sb-block-size sb) (bytes-u32-ref sb 8))
  (define (sb-next-free sb)  (bytes-u64-ref sb 32))
  (define (sb-next-id sb)    (bytes-u64-ref sb 40))
  (define (sb-obj-count sb)  (bytes-u64-ref sb 48))

  ;; A fresh superblock block (format): magic + the bump cursors at their start.
  (define (make-super bs)
    (let ((sb (make-bytes bs)))                    ; make-bytes zero-fills
      (let loop ((i 0) (m CARDFS-MAGIC))
        (if (not (null? m))
            (begin (bytes-u8-set! sb i (car m)) (loop (+ i 1) (cdr m)))))
      (bytes-u32-set! sb 8  bs)
      (bytes-u32-set! sb 12 TABLE-BLOCKS)
      (bytes-u64-set! sb 16 TABLE-LBA)
      (bytes-u64-set! sb 24 DATA-START)
      (bytes-u64-set! sb 32 DATA-START)            ; next_free
      (bytes-u64-set! sb 40 1)                      ; next_obj_id
      (bytes-u64-set! sb 48 0)                      ; obj_count
      sb))

  ;; --- object-table entries (128 bytes) ---------------------------------------
  ;; Within an entry at byte offset `off`: id@0 key@8(64) data_lba@72 size@80 valid@88.
  (define (entry-valid? blk off)   (not (= 0 (bytes-u32-ref blk (+ off 88)))))
  (define (entry-data-lba blk off) (bytes-u64-ref blk (+ off 72)))
  (define (entry-size blk off)     (bytes-u64-ref blk (+ off 80)))
  (define (per-block bs)           (quotient bs ENTRY-SIZE))

  ;; #t if the NUL-padded 64-byte key at off+8 equals string `key` (strncmp-style).
  (define (entry-key=? blk off key)
    (let ((n (string-length key)))
      (and (or (>= n KEY-LEN) (= 0 (bytes-u8-ref blk (+ off 8 n))))
           (let loop ((i 0))
             (cond ((>= i n) #t)
                   ((= (bytes-u8-ref blk (+ off 8 i)) (char->integer (string-ref key i)))
                    (loop (+ i 1)))
                   (else #f))))))

  ;; Write `key` (NUL-padded to KEY-LEN) into entry at off+8.
  (define (set-entry-key! blk off key)
    (let ((n (string-length key)))
      (let loop ((i 0))
        (if (< i KEY-LEN)
            (begin
              (bytes-u8-set! blk (+ off 8 i)
                             (if (< i n) (char->integer (string-ref key i)) 0))
              (loop (+ i 1)))))))

  ;; --- the provider context ---------------------------------------------------
  ;; A restricted context (no capabilities -- all block I/O goes through a storage
  ;; handle a message carries). The block I/O helpers and the operations live HERE,
  ;; closing over `stash` (the deferred-request FIFO) so overlapping requests
  ;; serialise without threading it through every call. Messages:
  ;;   (probe name bsize bcount driver storage) -- auto-mount: read LBA 0, claim on a
  ;;       cardfs magic, else decline (read-only -- never formats implicitly).
  ;;   (format storage name reply)          -> reply (complete status)
  ;;   (put    storage name key data reply) -> reply (complete status)
  ;;   (get    storage name key reply)      -> reply (got bytes-or-#f)
  (define (start-cardfs storage)
    (let ((prov
            (spawn-restricted '()
              (lambda ()
                (let ((stash '()))   ; FIFO of requests deferred during a block I/O

                  ;; Wait for the (complete ...) reply to an in-flight block I/O,
                  ;; deferring any other message; once the mailbox drains, recv
                  ;; blocks and the driver gets to run (no spin).
                  (define (io-recv)
                    (let loop ()
                      (let ((m (recv)))
                        (if (eq? (car m) 'complete)
                            m
                            (begin (set! stash (append stash (list m))) (loop))))))
                  (define (blk-read storage name lba)
                    (send storage (list 'read name lba 1 (self)))
                    (let ((m (io-recv))) (if (= (cadr m) 0) (caddr m) #f)))
                  (define (blk-write storage name lba blk)
                    (send storage (list 'write name lba 1 blk (self)))
                    (let ((m (io-recv))) (if (= (cadr m) 0) 0 -1)))

                  ;; format: zero the table, then write a fresh superblock. -> 0.
                  (define (do-format storage name)
                    (let ((zero (make-bytes DEFAULT-BS)))
                      (let loop ((i 0))
                        (if (< i TABLE-BLOCKS)
                            (begin (blk-write storage name (+ TABLE-LBA i) zero)
                                   (loop (+ i 1))))))
                    (blk-write storage name 0 (make-super DEFAULT-BS)))

                  ;; put: bump-allocate data blocks, fill a free table slot, commit
                  ;; the superblock. -> 0 / -1.
                  (define (do-put storage name key data)
                    (let ((sb (blk-read storage name 0)))
                      (if (or (not sb) (not (magic-ok? sb)))
                          -1
                          (let* ((bs (sb-block-size sb))
                                 (len (bytes-length data))
                                 (need (max 1 (quotient (+ len bs -1) bs)))
                                 (data-lba (sb-next-free sb)))
                            (let wloop ((i 0))
                              (if (< i need)
                                  (let* ((off (* i bs)) (chunk (min bs (- len off)))
                                         (blk (make-bytes bs)))
                                    (if (> chunk 0)
                                        (bytes-copy-into! blk 0 (copy-bytes data off chunk) chunk))
                                    (blk-write storage name (+ data-lba i) blk)
                                    (wloop (+ i 1)))))
                            (if (not (place-entry storage name sb bs key data-lba len))
                                -1
                                (begin
                                  (bytes-u64-set! sb 32 (+ data-lba need))
                                  (bytes-u64-set! sb 40 (+ (sb-next-id sb) 1))
                                  (bytes-u64-set! sb 48 (+ (sb-obj-count sb) 1))
                                  (blk-write storage name 0 sb)
                                  0))))))

                  ;; Fill the first invalid table slot; write that block back. #t/#f.
                  (define (place-entry storage name sb bs key data-lba len)
                    (let ((pb (per-block bs)))
                      (let tloop ((tb 0))
                        (if (>= tb TABLE-BLOCKS)
                            #f
                            (let ((blk (blk-read storage name (+ TABLE-LBA tb))))
                              (let eloop ((e 0))
                                (cond
                                  ((>= e pb) (tloop (+ tb 1)))
                                  ((not (entry-valid? blk (* e ENTRY-SIZE)))
                                   (let ((off (* e ENTRY-SIZE)))
                                     (bytes-u64-set! blk off (sb-next-id sb))
                                     (set-entry-key! blk off key)
                                     (bytes-u64-set! blk (+ off 72) data-lba)
                                     (bytes-u64-set! blk (+ off 80) len)
                                     (bytes-u32-set! blk (+ off 88) 1)
                                     (blk-write storage name (+ TABLE-LBA tb) blk)
                                     #t))
                                  (else (eloop (+ e 1))))))))))

                  ;; get: scan the table for key, read + return its exact bytes / #f.
                  (define (do-get storage name key)
                    (let ((sb (blk-read storage name 0)))
                      (if (or (not sb) (not (magic-ok? sb)))
                          #f
                          (let* ((bs (sb-block-size sb)) (hit (find-entry storage name bs key)))
                            (if (not hit)
                                #f
                                (let* ((data-lba (car hit)) (size (cadr hit))
                                       (need (max 1 (quotient (+ size bs -1) bs)))
                                       (out (make-bytes size)))
                                  (let rloop ((i 0))
                                    (if (< i need)
                                        (let* ((off (* i bs)) (chunk (min bs (- size off)))
                                               (blk (blk-read storage name (+ data-lba i))))
                                          (if (and blk (> chunk 0))
                                              (bytes-copy-into! out off blk chunk))
                                          (rloop (+ i 1)))))
                                  out))))))

                  ;; Scan table blocks for key; returns (data-lba size) or #f.
                  (define (find-entry storage name bs key)
                    (let ((pb (per-block bs)))
                      (let tloop ((tb 0))
                        (if (>= tb TABLE-BLOCKS)
                            #f
                            (let ((blk (blk-read storage name (+ TABLE-LBA tb))))
                              (let eloop ((e 0))
                                (cond
                                  ((>= e pb) (tloop (+ tb 1)))
                                  ((and (entry-valid? blk (* e ENTRY-SIZE))
                                        (entry-key=? blk (* e ENTRY-SIZE) key))
                                   (list (entry-data-lba blk (* e ENTRY-SIZE))
                                         (entry-size blk (* e ENTRY-SIZE))))
                                  (else (eloop (+ e 1))))))))))

                  ;; The dispatch loop: a deferred request first, else fresh input.
                  (let loop ()
                    (let ((m (if (null? stash)
                                 (recv)
                                 (let ((h (car stash))) (set! stash (cdr stash)) h))))
                      (cond
                        ((eq? (car m) 'probe)
                         (let* ((name (cadr m)) (stor (nth m 5))
                                (sb (blk-read stor name 0)))
                           (if (and sb (magic-ok? sb))
                               (begin
                                 (display "[cardfs] CARDFS volume on ") (display name)
                                 (display " (objs=") (display (sb-obj-count sb)) (display ")")
                                 (newline)
                                 (send stor (list 'claim name)))
                               (begin (display "[cardfs] no cardfs on ") (display name)
                                      (newline)))))
                        ((eq? (car m) 'format)
                         (send (nth m 3) (list 'complete (do-format (cadr m) (caddr m)))))
                        ((eq? (car m) 'put)
                         (send (nth m 5)
                               (list 'complete (do-put (cadr m) (caddr m) (cadddr m) (nth m 4)))))
                        ((eq? (car m) 'get)
                         (send (nth m 4)
                               (list 'got (do-get (cadr m) (caddr m) (cadddr m)))))
                        (else 'ignore))
                      (loop))))))))
      (send storage (list 'register-fsprovider 'cardfs prov))
      prov)))
