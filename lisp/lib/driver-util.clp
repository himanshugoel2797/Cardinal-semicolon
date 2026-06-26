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
          wait-until wait-until-spin serve reply-to
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

  ;; Poll `pred` until it is true, or `timeout-ns` elapses, busy-spinning for the
  ;; first `spin-ns` before falling back to a yielding (sleep) poll; #t if it became
  ;; true, #f on timeout. The spin catches sub-quantum operations -- a paravirt
  ;; virtio controlq command completes in microseconds, and (sleep 200000) actually
  ;; deschedules for at least a quantum, so a pure sleep-poll pays a chunk of latency
  ;; PER command (busy-polling ~500us first cut a virtio-gpu flush from ~1560us to
  ;; ~450us). After the spin budget it YIELDS (the core runs other contexts; a
  ;; boot-time direct eval with nothing to yield to spins via the counter fallback),
  ;; so genuine ms-scale waits (resets, link settle) don't burn the CPU.
  (define (wait-until-spin pred timeout-ns spin-ns)
    (let ((deadline (+ (uptime-ns) timeout-ns))
          (spin-deadline (+ (uptime-ns) spin-ns)))
      (let spin ()
        (cond ((pred) #t)
              ((> (uptime-ns) spin-deadline)
               (let loop ()
                 (cond ((pred) #t)
                       ((> (uptime-ns) deadline) #f)
                       (else (sleep 200000) (loop)))))
              (else (spin))))))

  ;; The plain device-bring-up wait: NO busy-spin (spin budget 0). Callers are
  ;; ms-scale reset / link-settle polls (hdaudio, rtl8139/69, ahci) that take the
  ;; full timeout to resolve, so a spin would only burn CPU before yielding. A
  ;; latency-sensitive sub-quantum path (the virtio controlq) calls wait-until-spin.
  (define (wait-until pred timeout-ns) (wait-until-spin pred timeout-ns 0))

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

  ;; A mutable single-value box. Backed by a 1-element VECTOR, not a bytes buffer:
  ;; the old bytes-u64 version could hold ONLY fixnums (bytes-set! rejects non-
  ;; fixnums) and was a GC leaf (a stored heap pointer was neither a real value nor
  ;; traced, so the GC freed it under the cell). Mutable vectors (the VM's general
  ;; mutable slot) hold any value and are GC-traced, so a cell can now safely hold a
  ;; list/string/bytes as well as a fixnum.
  (define (make-cell v) (make-vector 1 v))
  (define (cell-ref c)  (vector-ref c 0))
  (define (cell-set! c v) (vector-set! c 0 v))

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
          (loop (step state (recv)))))))

  ;; Reply to a request whose reply address arrived INSIDE an (untrusted) message.
  ;; Deliver only if `target` is a context. The VM has no try/catch, so a `send` to
  ;; a non-context aborts the calling context -- and a `serve` loop has no recovery,
  ;; so a forged/garbage reply handle from any sender would otherwise permanently
  ;; kill the service. `ctx?` makes the type checkable; `reply-to` is the one-liner
  ;; every request/reply server uses instead of a bare `send` to a message field.
  ;; Returns #t if delivered. (For a reply handle being FORWARDED into another
  ;; message rather than sent to, guard the forward with `(ctx? handle)` directly --
  ;; the eventual sender is what must be protected.)
  (define (reply-to target msg)
    (if (ctx? target) (begin (send target msg) #t) #f)))
