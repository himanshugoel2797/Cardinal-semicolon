;; virtio-rng: the virtio 1.0 entropy-source driver (PCI 1af4:1044).
;;
;; The simplest possible virtio device: ONE virtqueue (index 0), no device
;; config space, no feature bits beyond VERSION_1. The driver posts device-
;; WRITABLE buffers onto the queue; the device fills each with random bytes and
;; returns it on the used ring, where the used `len` is the count of bytes it
;; actually wrote (it may be fewer than the buffer size).
;;
;; Above that raw queue the driver runs a small entropy SERVICE context: send it
;; (list 'get-random N reply) and it answers (send reply (list 'random <N bytes>)).
;; It refills from the queue as needed and buffers any leftover bytes from a
;; chunk so a later request can drain them without another device round-trip.
;;
;; The pure half -- how a stream of filled chunks is sliced into the next N
;; requested bytes -- is factored into `rng-take` so it is unit-testable on the
;; host with no device at all.
(define-module virtio-rng
  (export rng-init rng-take)
  (import sys-mmio sys-pci driver-util virtio)

;; --- pure: assemble N bytes out of a buffered byte list --------------------
;; A "pool" is a (list <bytes> <fill-len> <consumed>): a chunk buffer, how many
;; valid bytes it holds, and how many of those have already been handed out.
;; rng-take pulls up to `want` bytes from the pool into `out` starting at
;; `out-off`; it returns (list <new-consumed> <bytes-copied>). It never blocks
;; and never refills -- the caller decides when the pool is exhausted (consumed
;; = fill) and a new chunk must be fetched. Keeping this side-effect-free (apart
;; from the destructive copy into the caller-owned `out`) is what makes the
;; refill/slice logic host-testable.
(define (rng-take pool out out-off want)
  (let ((buf      (nth pool 0))
        (fill     (nth pool 1))
        (consumed (nth pool 2)))
    (let ((avail (- fill consumed)))
      (let ((take (if (< avail want) avail want)))
        (if (= take 0)
            (list consumed 0)
            (begin
              (bytes-copy! out out-off buf consumed take)
              (list (+ consumed take) take)))))))

;; --- device-backed entropy queue -------------------------------------------

(define RNG-CHUNK 64)        ; bytes pulled from the device per round-trip
(define RNG-TIMEOUT 2000000) ; 2ms: the device answers in microseconds

;; Post a fresh device-WRITABLE buffer on descriptor 0 of the rng queue, kick,
;; and wait for the device to fill it. Returns the number of bytes written
;; (the used `len`), or 0 on timeout. `rngbuf` is reused each round.
(define (rng-pull! q notify mult rngbuf)
  (let ((desc (q-desc q)) (avail (q-avail q)) (qsize (q-size q))
        (last (bytes-u16-ref (q-used q) 2)))
    (desc-set! desc 0 (bytes-phys rngbuf) (bytes-length rngbuf) VIRTQ-DESC-F-WRITE 0)
    (avail-push! avail qsize 0)
    (notify-queue! notify mult q)
    (if (wait-until-spin (lambda () (used-advanced? q last)) RNG-TIMEOUT 500000)
        ;; used ring slot for the entry we just snapshotted past `last`.
        (let ((slot (modulo last qsize)))
          (bytes-u32-ref (q-used q) (+ 8 (* 8 slot))))   ; used.len = bytes written
        0)))

;; Fill `out[0..n)` with device entropy, looping rng-pull! as many times as the
;; chunk size requires. `pool` is a (make-cell <pool-list>) carrying leftover
;; bytes between requests so small requests don't each cost a round-trip.
(define (rng-fill! q notify mult rngbuf pool out n)
  (let loop ((off 0))
    (if (>= off n)
        out
        (let ((p (cell-ref pool)))
          ;; refill the pool from the device when it is drained.
          (if (>= (nth p 2) (nth p 1))
              (let ((len (rng-pull! q notify mult rngbuf)))
                (cell-set! pool (list (copy-bytes rngbuf 0 len) len 0))
                (loop off))
              (let ((r (rng-take p out off (- n off))))
                (cell-set! pool (list (nth p 0) (nth p 1) (nth r 0)))
                (loop (+ off (nth r 1)))))))))

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-RNG-VID #x1af4)
(define VIRTIO-RNG-DID #x1044)

;; rng-init brings the device up and spawns the entropy service, returning the
;; service handle (or #f). It runs at init time -- NOT under the scheduler -- so
;; it must not block; it spawns the service context and returns immediately.
(define (rng-init ecam)
  (if (not ecam)
      (begin (display "[virtio-rng] no device present") (newline) #f)
      ;; Only VERSION_1 -- the rng device offers no other useful feature.
      (let ((dev (virtio-bringup ecam 0 (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
        (if (not dev)
            (begin (display "[virtio-rng] device rejected FEATURES_OK") (newline) #f)
            (let ((common (nth dev 0)) (notify (nth dev 2)) (mult (nth dev 3)))
              (let ((q (virtio-setup-queue common 0)))
                (if (not q)
                    (begin (display "[virtio-rng] no requestq") (newline) #f)
                    (begin
                      (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                      (display "[virtio-rng] up") (newline)
                      (let ((rngbuf (dma-alloc RNG-CHUNK))
                            (pool   (make-cell (list (make-bytes 0) 0 0))))
                        ;; The service answers (get-random N reply). It runs with
                        ;; the empty grant (serve) but closes over the queue caps
                        ;; it needs; a forged reply handle is guarded by reply-to.
                        (serve 'rng
                          (lambda (state m)
                            (if (and (pair? m) (eq? (car m) 'get-random)
                                     (pair? (cdr m)) (integer? (cadr m)))
                                (let ((n (cadr m)) (reply (nth m 2)))
                                  (if (and (>= n 0) (<= n 4096) (ctx? reply))
                                      (let ((out (make-bytes (if (= n 0) 1 n))))
                                        (rng-fill! q notify mult rngbuf pool out n)
                                        (reply-to reply
                                          (list 'random (copy-bytes out 0 n))))
                                      (reply-to reply (list 'random-error 'bad-request)))
                                  state)
                                state)))))))))))))
