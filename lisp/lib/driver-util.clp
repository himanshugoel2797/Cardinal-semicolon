;; driver-util: generic helpers shared by Lisp device drivers.
;;
;; The first Cardinal Lisp *library* -- a module (define-module) rather than a
;; flat top-level program. Drivers pull these in with (import driver-util)
;; instead of redefining them per file. Everything here is device-agnostic:
;; list indexing and a mutable word cell built on the byte-buffer primitive
;; (the language's pairs/vectors are immutable, so a 1-element bytes buffer is
;; the mutable store the driver substrate needs).

(define-module driver-util
  (export nth make-cell cell-ref cell-set!
          put-be16! get-be16 put-be32! get-be32
          copy-bytes bytes-copy-into! put-list!
          wait-until serve
          PCI-COMMAND bar-base pci-enable-mem-bus-master!)

  (define (nth lst k) (if (= k 0) (car lst) (nth (cdr lst) (- k 1))))

  ;; --- generic PCI config-space plumbing -------------------------------------
  ;; Shared by every PCI driver (virtio, rtl8139, and the coming rtl8169/ahci):
  ;; the COMMAND register and BAR base decode live here, not in any one driver.

  (define PCI-COMMAND #x04)   ; u16: bit1 = memory space, bit2 = bus master

  ;; Resolve a BAR's base physical address (handles 64-bit memory BARs). cfg is the
  ;; mapped ECAM config space; bar-idx selects BAR0..5.
  (define (bar-base cfg bar-idx)
    (let* ((off (+ #x10 (* bar-idx 4)))
           (lo  (bytes-u32-ref cfg off)))
      (if (= (bit-extract lo 1 2) 2)
          (+ (bitwise-and lo #xFFFFFFF0)
             (arithmetic-shift (bytes-u32-ref cfg (+ off 4)) 32))
          (bitwise-and lo #xFFFFFFF0))))

  ;; Enable memory-space decoding + bus mastering (COMMAND bits 1|2) -- the two
  ;; bits every memory-mapped, DMA-capable PCI device needs set before use.
  (define (pci-enable-mem-bus-master! cfg)
    (bytes-u16-set! cfg PCI-COMMAND
                    (bitwise-or (bytes-u16-ref cfg PCI-COMMAND) #x6)))

  ;; Poll `pred` until it is true, or `timeout-ns` elapses; #t if it became true,
  ;; #f on timeout. The device-bring-up analogue of the C drivers' timer_timeout
  ;; loops (reset/link settle polls). It YIELDS between polls via (sleep): under
  ;; the scheduler the core runs other contexts while we wait; only a boot-time
  ;; direct eval (no context to yield to) falls back to a counter wait. The poll
  ;; interval bounds how often pred is checked -- ~200us, fine for ms-scale waits.
  (define (wait-until pred timeout-ns)
    (let ((deadline (+ (uptime-ns) timeout-ns)))
      (let loop ()
        (cond ((pred) #t)
              ((> (uptime-ns) deadline) #f)
              (else (sleep 200000) (loop))))))

  ;; Copy `len` bytes out of `src` starting at `off` into a fresh owned buffer.
  ;; The NIC RX path needs this: the device's receive buffer is recycled, so a
  ;; frame handed to the network stack must be snapshotted first.
  (define (copy-bytes src off len)
    (let ((out (make-bytes len)))
      (bytes-copy! out 0 src off len)
      out))

  ;; Copy `len` bytes from src[0..) into dst at `off` (in place).
  (define (bytes-copy-into! dst off src len)
    (bytes-copy! dst off src 0 len)
    dst)

  ;; Write a list of byte values into `b` starting at `off`.
  (define (put-list! b off lst)
    (let loop ((i off) (l lst))
      (if (null? l) b (begin (bytes-u8-set! b i (car l)) (loop (+ i 1) (cdr l))))))

  (define (make-cell v) (let ((b (make-bytes 8))) (bytes-u64-set! b 0 v) b))
  (define (cell-ref c)  (bytes-u64-ref c 0))
  (define (cell-set! c v) (bytes-u64-set! c 0 v))

  ;; Network byte order (big-endian) read/write into a byte buffer. The volatile
  ;; byte accessors are little-endian-native, so protocol headers (which are
  ;; big-endian) are laid out a byte at a time. virtio-net grew its own copies of
  ;; these; they live here now so every networking module shares one definition.
  (define (put-be16! b off v)
    (bytes-u8-set! b off       (bit-extract v 8 8))
    (bytes-u8-set! b (+ off 1) (bit-extract v 0 8)))
  (define (get-be16 b off)
    (bitwise-or (arithmetic-shift (bytes-u8-ref b off) 8)
                (bytes-u8-ref b (+ off 1))))
  (define (put-be32! b off v)
    (put-be16! b off       (bit-extract v 16 16))
    (put-be16! b (+ off 2) (bit-extract v 0 16)))
  (define (get-be32 b off)
    (bitwise-or (arithmetic-shift (get-be16 b off) 16)
                (get-be16 b (+ off 2))))

  ;; The server mold. Cardinal's OS services are long-lived restricted contexts
  ;; that own some state and a message loop: `send` to them, never call them
  ;; synchronously, so the rx-handler-re-enters-tx self-deadlock that plagued the
  ;; C servers cannot arise. `serve` captures that shape once: it spawns a context
  ;; with the empty capability grant (a wedged/compromised service can't acquire
  ;; new authority -- the least-privilege posture) running a loop that threads
  ;; `state`: each message m becomes (step state m) -> next-state. The handler
  ;; closes over whatever capabilities its defining module imported. Returns the
  ;; context handle callers `send` to.
  (define (serve init step)
    (spawn-restricted '()
      (lambda ()
        (let loop ((state init))
          (loop (step state (recv))))))))
