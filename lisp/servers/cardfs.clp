;; cardfs: a crash-consistent, integrity-checked object store (CoreStorage fs
;; provider + flat key->blob map). Rewritten from the in-place flat-table version
;; into a LOG-STRUCTURED / copy-on-write design so that a power loss can never
;; leave a half-applied mutation visible, and bit-rot / torn writes are DETECTED
;; rather than silently served as good data. This is the small, robust expression
;; of the Main.md direction (checkpoint blocks blk0/blk1 + generation + COW).
;;
;; ON-DISK LAYOUT (512-byte blocks; single-device, single block size assumed):
;;   LBA 0, LBA 1 : two superblock "checkpoint" slots (A / B). Each is a complete
;;                  root: {magic, version, block_size, generation, log_start,
;;                  log_head, next_id, obj_count, crc32}. The slot with the HIGHEST
;;                  generation whose crc32 validates is the live root.
;;   LBA 2 ..     : an append-only LOG of records. Each record is
;;                  [header block | payload block ...]; the header carries
;;                  {magic, type(put/tombstone), id, size, nblocks, keylen, key,
;;                   payload_crc32, header_crc32}. Records are never overwritten
;;                  while live -- a put/delete APPENDS a new record at log_head.
;;
;; WHY THIS IS CRASH-SAFE. A mutation is two steps: (1) append the record into
;; free space past log_head (the live root does not point at it yet, so a torn
;; write there is invisible), then (2) write a NEW superblock -- gen+1, advanced
;; log_head -- into the *inactive* slot. Step 2 is the single atomic commit: a
;; torn write to the inactive slot fails its crc32, so mount falls back to the
;; still-valid other slot (the pre-commit state, fully intact because the log is
;; append-only). So a crash is all-or-nothing: either the whole record commits or
;; none of it is visible. On mount we pick the newest valid superblock and REPLAY
;; the log [log_start, log_head) to rebuild the in-RAM key->entry index; a put
;; record (re)binds a key, a tombstone unbinds it (last writer wins). Every record
;; header is crc32-checked during replay, and every object's payload is
;; crc32-checked on get -- a corrupt object is reported, never served.
;;
;; WHAT IS DELIBERATELY ABSENT (future, not this slice): log compaction / GC (the
;; log grows until the device is full, then put returns 'full), multi-device, and
;; a write barrier/cache-flush primitive -- we rely on the storage path completing
;; each block write before its ack (blk-write waits for 'complete), which orders
;; the record before its commit superblock. A device that reorders writes across
;; that ack could, in the worst case, truncate the log at a torn record on replay;
;; it still never serves corrupt data. See notes/servers/CoreStorage.
;;
;; I/O is by MESSAGE over the CoreStorage protocol (a provider reads a block by
;; sending (read name lba count self) to the storage handle and receiving
;; (complete status bytes)). cardfs is a single context, so it does that round-
;; trip SYNCHRONOUSLY inside a request handler; a NEW request that lands mid-I/O is
;; DEFERRED into `stash` and re-processed by the main loop, so overlapping requests
;; serialise through this one context. Message API (see the dispatch loop):
;;   (probe   name bsize bcount driver storage) -- auto-mount on a valid CARDLOG
;;       volume (claims it), else decline (read-only; never formats implicitly).
;;   (format    storage name bcount reply)        -> (complete ok|io-error)
;;   (put       storage name key data reply)      -> (complete ok|full|io-error|no-volume)
;;   (get       storage name key reply)           -> (got ok bytes | miss #f | corrupt #f | no-volume #f)
;;   (get-range storage name key off len reply)   -> (got ok bytes | miss #f | range #f | corrupt #f | no-volume #f)
;;   (delete    storage name key reply)           -> (complete ok|miss|io-error|no-volume)
;;   (stat      storage name key reply)           -> (stat (size id) | #f | no-volume)
;;   (keys      storage name reply)               -> (keys (key ...) | no-volume)

(define-module cardfs
  (export start-cardfs)
  (import driver-util)
  (define lg (make-logger 'cardfs))

  (define BS         512)                              ; block size (bytes)
  (define LOG-START  2)                                ; first log LBA (after 2 SB slots)
  (define KEY-LEN    64)                               ; max key length (bytes)
  (define VERSION    1)
  (define TYPE-PUT   1)
  (define TYPE-TOMB  2)
  (define LOG-MAGIC  (list 67 65 82 68 76 79 71 49))   ; "CARDLOG1" (superblock)
  (define REC-MAGIC  #x43524543)                       ; "CREC" (record header)

  (define (ceil-div a b) (quotient (+ a b -1) b))
  (define (other-slot s) (- 1 s))

  ;; --- crc32 (IEEE 802.3, reflected, poly 0xEDB88320) -------------------------
  ;; Pure Lisp -- no VM primitive. A 256-entry table is built once at load so a
  ;; 512-byte block costs 512 table lookups, not 4096 bit ops.
  (define crc32-table
    (let ((t (make-vector 256 0)))
      (let loop ((n 0))
        (if (>= n 256)
            t
            (begin
              (vector-set! t n
                (let cloop ((c n) (k 0))
                  (if (>= k 8)
                      c
                      (cloop (if (= 1 (bitwise-and c 1))
                                 (bitwise-xor #xEDB88320 (arithmetic-shift c -1))
                                 (arithmetic-shift c -1))
                             (+ k 1)))))
              (loop (+ n 1)))))))

  (define (crc32 b off len)
    (let loop ((i 0) (crc #xFFFFFFFF))
      (if (>= i len)
          (bitwise-xor crc #xFFFFFFFF)
          (loop (+ i 1)
                (bitwise-xor (arithmetic-shift crc -8)
                             (vector-ref crc32-table
                                         (bitwise-and #xFF (bitwise-xor crc (bytes-u8-ref b (+ off i))))))))))

  ;; --- byte-field helpers -----------------------------------------------------
  (define (write-magic b off m)
    (let loop ((i 0) (rest m))
      (if (not (null? rest))
          (begin (bytes-u8-set! b (+ off i) (car rest)) (loop (+ i 1) (cdr rest))))))
  (define (magic-ok? b m)
    (and (>= (bytes-length b) 8)
         (let loop ((i 0) (rest m))
           (cond ((null? rest) #t)
                 ((= (bytes-u8-ref b i) (car rest)) (loop (+ i 1) (cdr rest)))
                 (else #f)))))
  (define (crc-off) (- BS 4))

  ;; Canonical index key: the on-disk key field is NUL-padded to KEY-LEN, so a key
  ;; longer than KEY-LEN is truncated -- index by the truncated form in BOTH put
  ;; and replay so a remount finds what a put stored.
  (define (key-canon key)
    (let ((n (string-length key)))
      (if (<= n KEY-LEN)
          key
          (let loop ((i 0) (acc '()))
            (if (>= i KEY-LEN)
                (list->string (reverse acc))
                (loop (+ i 1) (cons (string-ref key i) acc)))))))
  (define (set-key! b off key)
    (let ((n (string-length key)))
      (let loop ((i 0))
        (if (< i KEY-LEN)
            (begin (bytes-u8-set! b (+ off i) (if (< i n) (char->integer (string-ref key i)) 0))
                   (loop (+ i 1)))))))
  (define (read-key b off n)
    (let ((nn (min n KEY-LEN)))
      (let loop ((i 0) (acc '()))
        (if (>= i nn)
            (list->string (reverse acc))
            (loop (+ i 1) (cons (integer->char (bytes-u8-ref b (+ off i))) acc))))))

  ;; --- superblock (fields are native-endian, matching bytes-uNN) --------------
  ;; magic@0(8) version@8(u32) block_size@12(u32) generation@16(u64) log_start@24
  ;; log_head@32 next_id@40 obj_count@48 ... crc32@(BS-4).
  (define (sb-gen sb)       (bytes-u64-ref sb 16))
  (define (sb-log-start sb) (bytes-u64-ref sb 24))
  (define (sb-log-head sb)  (bytes-u64-ref sb 32))
  (define (sb-next-id sb)   (bytes-u64-ref sb 40))
  (define (sb-obj-count sb) (bytes-u64-ref sb 48))
  (define (seal-sb! sb)     (bytes-u32-set! sb (crc-off) (crc32 sb 0 (crc-off))))
  (define (sb-valid? sb)
    (and (= (bytes-length sb) BS)
         (magic-ok? sb LOG-MAGIC)
         (= VERSION (bytes-u32-ref sb 8))
         (= BS (bytes-u32-ref sb 12))
         (= (bytes-u32-ref sb (crc-off)) (crc32 sb 0 (crc-off)))))
  (define (make-fresh-sb)
    (let ((sb (make-bytes BS)))
      (write-magic sb 0 LOG-MAGIC)
      (bytes-u32-set! sb 8 VERSION)
      (bytes-u32-set! sb 12 BS)
      (bytes-u64-set! sb 16 1)            ; generation
      (bytes-u64-set! sb 24 LOG-START)
      (bytes-u64-set! sb 32 LOG-START)    ; log_head (empty log)
      (bytes-u64-set! sb 40 1)            ; next_id
      (bytes-u64-set! sb 48 0)            ; obj_count
      (seal-sb! sb)
      sb))
  ;; A fresh superblock copied from `old` with the commit fields advanced.
  (define (next-sb old new-head new-next-id new-count)
    (let ((sb (make-bytes BS)))
      (bytes-copy-into! sb 0 old BS)
      (bytes-u64-set! sb 16 (+ (sb-gen old) 1))
      (bytes-u64-set! sb 32 new-head)
      (bytes-u64-set! sb 40 new-next-id)
      (bytes-u64-set! sb 48 new-count)
      (seal-sb! sb)
      sb))

  ;; --- record header (HDR-KEY-OFF..+KEY-LEN holds the NUL-padded key) ----------
  ;; magic@0(u32) type@4(u32) id@8(u64) size@16(u64) keylen@24(u32) nblocks@28(u32)
  ;; payload_crc@32(u32) key@40(KEY-LEN) ... crc32@(BS-4).
  (define HDR-KEY-OFF 40)
  (define (seal-hdr! h) (bytes-u32-set! h (crc-off) (crc32 h 0 (crc-off))))
  (define (hdr-valid? h)
    (and (= REC-MAGIC (bytes-u32-ref h 0))
         (= (bytes-u32-ref h (crc-off)) (crc32 h 0 (crc-off)))))

  ;; --- in-RAM volume state ----------------------------------------------------
  ;; A mutable vector #(active-slot superblock index bcount). `index` is a hash
  ;; table key-string -> (data-lba size payload-crc id). active-slot/superblock are
  ;; mutated in place on each commit.
  (define (vol-active v) (vector-ref v 0))
  (define (vol-sb v)     (vector-ref v 1))
  (define (vol-index v)  (vector-ref v 2))
  (define (vol-bcount v) (vector-ref v 3))
  (define (ent-lba e)  (car e))
  (define (ent-size e) (cadr e))
  (define (ent-crc e)  (caddr e))
  (define (ent-id e)   (cadddr e))

  ;; --- the provider context ---------------------------------------------------
  ;; A restricted context (no capabilities -- all block I/O rides a storage handle
  ;; a message carries). Closes over `vols` (name -> volume state) and `stash`
  ;; (requests deferred during an in-flight block I/O).
  (define (start-cardfs storage)
    (let ((prov
            (spawn-restricted '()
              (lambda ()
                (let ((vols  (make-hash-table))
                      (stash '()))

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

                  ;; Read both superblock slots, pick the newest valid one, replay
                  ;; the log to build the index. Returns a volume vector or #f (no
                  ;; valid CARDLOG volume). Pure read -- never mutates the device.
                  (define (mount storage name bcount)
                    (let* ((sb0 (blk-read storage name 0))
                           (sb1 (blk-read storage name 1))
                           (v0  (and sb0 (sb-valid? sb0)))
                           (v1  (and sb1 (sb-valid? sb1))))
                      (if (not (or v0 v1))
                          #f
                          (let* ((use1 (cond ((not v0) #t)
                                             ((not v1) #f)
                                             (else (> (sb-gen sb1) (sb-gen sb0)))))
                                 (sb (if use1 sb1 sb0))
                                 (index (make-hash-table)))
                            (replay storage name sb index)
                            (vector (if use1 1 0) sb index bcount)))))

                  ;; Replay [log_start, log_head): each valid put record (re)binds
                  ;; its key, each tombstone unbinds it. A header that fails its
                  ;; crc32 (torn/garbage) ends replay -- we cannot trust its nblocks
                  ;; to skip past, and the log is contiguous up to the committed
                  ;; head, so anything beyond a bad header is not committed state.
                  (define (replay storage name sb index)
                    (let ((head (sb-log-head sb)))
                      (let loop ((lba (sb-log-start sb)))
                        (if (>= lba head)
                            #t
                            (let ((h (blk-read storage name lba)))
                              (if (or (not h) (not (hdr-valid? h)))
                                  #t
                                  (let ((type (bytes-u32-ref h 4))
                                        (id   (bytes-u64-ref h 8))
                                        (size (bytes-u64-ref h 16))
                                        (klen (bytes-u32-ref h 24))
                                        (nblk (bytes-u32-ref h 28))
                                        (pcrc (bytes-u32-ref h 32)))
                                    ;; nblk must match size, else a crc32-colliding
                                    ;; header could skip/overlap records -- end replay.
                                    (if (not (= nblk (if (> size 0) (ceil-div size BS) 0)))
                                        #t
                                        (begin
                                          (let ((key (read-key h HDR-KEY-OFF klen)))
                                            (cond
                                              ((= type TYPE-PUT)
                                               (hash-set! index key (list (+ lba 1) size pcrc id)))
                                              ((= type TYPE-TOMB)
                                               (if (hash-has-key? index key) (hash-remove! index key)))))
                                          (loop (+ lba 1 nblk)))))))))))

                  ;; Append a record (header + payload) into free space, then flip
                  ;; the active superblock -- the single atomic commit. Returns
                  ;; (data-lba payload-crc) on success, or 'full / 'io-error.
                  (define (commit vol storage name type id key data new-next-id new-count)
                    (let* ((sb   (vol-sb vol))
                           (head (sb-log-head sb))
                           (size (if data (bytes-length data) 0))
                           (nblk (if (> size 0) (ceil-div size BS) 0))
                           (total (+ 1 nblk))
                           (pcrc (if (> size 0) (crc32 data 0 size) 0)))
                      (cond
                        ((> (+ head total) (vol-bcount vol)) 'full)
                        (else
                         (let ((h (make-bytes BS)))
                           (bytes-u32-set! h 0 REC-MAGIC)
                           (bytes-u32-set! h 4 type)
                           (bytes-u64-set! h 8 id)
                           (bytes-u64-set! h 16 size)
                           (bytes-u32-set! h 24 (min (string-length key) KEY-LEN))
                           (bytes-u32-set! h 28 nblk)
                           (bytes-u32-set! h 32 pcrc)
                           (set-key! h HDR-KEY-OFF key)
                           (seal-hdr! h)
                           (cond
                             ((not (= 0 (blk-write storage name head h))) 'io-error)
                             ((not (write-payload storage name (+ head 1) data size nblk)) 'io-error)
                             (else
                              ;; record durable -> flip the superblock (atomic commit)
                              (let ((nsb  (next-sb sb (+ head total) new-next-id new-count))
                                    (slot (other-slot (vol-active vol))))
                                (if (not (= 0 (blk-write storage name slot nsb)))
                                    'io-error
                                    (begin
                                      (vector-set! vol 0 slot)
                                      (vector-set! vol 1 nsb)
                                      (list (+ head 1) pcrc)))))))))))

                  (define (write-payload storage name lba data size nblk)
                    (let loop ((i 0))
                      (if (>= i nblk)
                          #t
                          (let* ((off (* i BS)) (chunk (min BS (- size off))) (blk (make-bytes BS)))
                            (if (> chunk 0)
                                (bytes-copy! blk 0 data off chunk))
                            (if (= 0 (blk-write storage name (+ lba i) blk))
                                (loop (+ i 1))
                                #f)))))

                  ;; format: invalidate slot B (so a stale higher-gen checkpoint
                  ;; can't win), then commit a fresh superblock to slot A. Not
                  ;; crash-atomic, but never corrupts: a crash leaves either the old
                  ;; volume or no volume, never a torn mix. -> 'ok / 'io-error.
                  (define (do-format storage name bcount)
                    (if (and (= 0 (blk-write storage name 1 (make-bytes BS)))
                             (let ((sb (make-fresh-sb)))
                               (and (= 0 (blk-write storage name 0 sb))
                                    (begin (hash-set! vols name (vector 0 sb (make-hash-table) bcount))
                                           #t))))
                        'ok
                        'io-error))

                  (define (do-put vol storage name key data)
                    (let* ((idx (vol-index vol)) (k (key-canon key)) (sb (vol-sb vol))
                           (existed (hash-has-key? idx k))
                           (id (sb-next-id sb))
                           (r (commit vol storage name TYPE-PUT id key data
                                      (+ id 1) (+ (sb-obj-count sb) (if existed 0 1)))))
                      (cond
                        ((eq? r 'full) 'full)
                        ((eq? r 'io-error) 'io-error)
                        (else (hash-set! idx k (list (car r) (bytes-length data) (cadr r) id)) 'ok))))

                  (define (do-delete vol storage name key)
                    (let* ((idx (vol-index vol)) (k (key-canon key)) (e (hash-ref idx k #f)))
                      (if (not e)
                          'miss
                          (let ((r (commit vol storage name TYPE-TOMB (ent-id e) k #f
                                           (sb-next-id (vol-sb vol))
                                           (- (sb-obj-count (vol-sb vol)) 1))))
                            (cond ((eq? r 'full) 'full)
                                  ((eq? r 'io-error) 'io-error)
                                  (else (hash-remove! idx k) 'ok))))))

                  ;; get: read the object's data blocks, verify the whole-payload
                  ;; crc32. -> (ok bytes) | (miss #f) | (corrupt #f).
                  (define (do-get vol storage name key)
                    (let ((e (hash-ref (vol-index vol) (key-canon key) #f)))
                      (if (not e)
                          (list 'miss #f)
                          (let* ((dlba (ent-lba e)) (size (ent-size e))
                                 (nblk (if (> size 0) (ceil-div size BS) 0))
                                 (out (make-bytes size)) (ok #t))
                            (let loop ((i 0))
                              (if (< i nblk)
                                  (let* ((off (* i BS)) (chunk (min BS (- size off)))
                                         (blk (blk-read storage name (+ dlba i))))
                                    (if blk
                                        (if (> chunk 0) (bytes-copy-into! out off blk chunk))
                                        (set! ok #f))
                                    (loop (+ i 1)))))
                            (cond ((not ok) (list 'corrupt #f))
                                  ((and (> size 0) (not (= (ent-crc e) (crc32 out 0 size)))) (list 'corrupt #f))
                                  (else (list 'ok out)))))))

                  ;; get-range: read only the blocks covering [off, off+len) and
                  ;; return that slice. NOTE: a partial slice cannot be checked
                  ;; against the whole-payload crc, so a ranged read is NOT
                  ;; integrity-verified (only a failed block read -> 'corrupt). Use
                  ;; (get) when you need the integrity guarantee.
                  (define (do-get-range vol storage name key off len)
                    (let ((e (hash-ref (vol-index vol) (key-canon key) #f)))
                      (cond
                        ((not e) (list 'miss #f))
                        ((< len 0) (list 'range #f))
                        ((or (< off 0) (> (+ off len) (ent-size e))) (list 'range #f))
                        ((= len 0) (list 'ok (make-bytes 0)))
                        (else
                         (let ((dlba (ent-lba e)) (out (make-bytes len)) (ok #t)
                               (fb (quotient off BS)) (lb (quotient (+ off len -1) BS)))
                           (let loop ((blk fb))
                             (if (> blk lb)
                                 #t
                                 (let ((data (blk-read storage name (+ dlba blk))))
                                   (if (not data)
                                       (set! ok #f)
                                       (let* ((bstart (* blk BS))
                                              (cstart (max off bstart))
                                              (cend (min (+ off len) (+ bstart BS)))
                                              (n (- cend cstart)))
                                         (if (> n 0)
                                             (bytes-copy! out (- cstart off) data (- cstart bstart) n))))
                                   (loop (+ blk 1)))))
                           (if ok (list 'ok out) (list 'corrupt #f)))))))

                  (define (do-stat vol key)
                    (let ((e (hash-ref (vol-index vol) (key-canon key) #f)))
                      (if e (list (ent-size e) (ent-id e)) #f)))

                  ;; The dispatch loop: a deferred request first, else fresh input.
                  (let loop ()
                    (let ((m (if (null? stash)
                                 (recv)
                                 (let ((h (car stash))) (set! stash (cdr stash)) h))))
                      (cond
                        ((eq? (car m) 'probe)               ; (probe name bsize bcount driver storage)
                         (let* ((name (cadr m)) (bcount (cadddr m)) (stor (nth m 5))
                                (vol (mount stor name bcount)))
                           (if vol
                               (begin
                                 (hash-set! vols name vol)
                                 (lg "CARDLOG volume on " name " (objs=" (hash-count (vol-index vol)) ")")
                                 (send stor (list 'claim name)))
                               (begin (lg "no cardlog on " name)))))
                        ((eq? (car m) 'format)              ; (format storage name bcount reply)
                         (reply-to (nth m 4) (list 'complete (do-format (cadr m) (caddr m) (cadddr m)))))
                        ((eq? (car m) 'put)                 ; (put storage name key data reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (nth m 5)
                                     (list 'complete
                                           (if vol (do-put vol (cadr m) (caddr m) (cadddr m) (nth m 4)) 'no-volume)))))
                        ((eq? (car m) 'delete)              ; (delete storage name key reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (nth m 4)
                                     (list 'complete
                                           (if vol (do-delete vol (cadr m) (caddr m) (cadddr m)) 'no-volume)))))
                        ((eq? (car m) 'get)                 ; (get storage name key reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (nth m 4)
                                     (if vol (cons 'got (do-get vol (cadr m) (caddr m) (cadddr m)))
                                         (list 'got 'no-volume #f)))))
                        ((eq? (car m) 'get-range)           ; (get-range storage name key off len reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (nth m 6)
                                     (if vol (cons 'got (do-get-range vol (cadr m) (caddr m) (cadddr m) (nth m 4) (nth m 5)))
                                         (list 'got 'no-volume #f)))))
                        ((eq? (car m) 'stat)                ; (stat storage name key reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (nth m 4)
                                     (list 'stat (if vol (do-stat vol (cadddr m)) 'no-volume)))))
                        ((eq? (car m) 'keys)                ; (keys storage name reply)
                         (let ((vol (hash-ref vols (caddr m) #f)))
                           (reply-to (cadddr m)
                                     (list 'keys (if vol (hash-keys (vol-index vol)) 'no-volume)))))
                        (else 'ignore))
                      (loop))))))))
      (send storage (list 'register-fsprovider 'cardfs prov))
      prov)))
