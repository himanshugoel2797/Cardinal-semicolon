;; xhci/regs -- xHCI register map, TRB type helpers, and the producer TRB ring.
;; Ported from drivers/xhci/inc/xhci.h + the ring helpers in main.c.
;;
;; All registers are MMIO; the driver maps one window and uses absolute offsets:
;; capability registers at base, operational at base+CAPLENGTH, runtime at
;; base+RTSOFF, the doorbell array at base+DBOFF. A ring is a DMA page of 64 TRBs
;; with a permanent Link TRB in the last slot pointing back to the start, and a
;; software cycle bit toggled on wrap.

;; ---- capability registers (offset from MMIO base) ----
(define XHCI-CAP-CAPLENGTH  #x00)   ; u8
(define XHCI-CAP-HCSPARAMS1 #x04)
(define XHCI-CAP-HCCPARAMS1 #x10)
(define XHCI-CAP-DBOFF      #x14)
(define XHCI-CAP-RTSOFF     #x18)

;; ---- operational registers (offset from `op`) ----
(define XHCI-OP-USBCMD #x00)
(define XHCI-OP-USBSTS #x04)
(define XHCI-OP-CRCR   #x18)   ; 64-bit
(define XHCI-OP-DCBAAP #x30)   ; 64-bit
(define XHCI-OP-CONFIG #x38)
(define (XHCI-OP-PORTSC p) (+ #x400 (* (- p 1) #x10)))   ; p is 1-based

(define XHCI-USBCMD-RS    1)
(define XHCI-USBCMD-HCRST 2)
(define XHCI-USBCMD-INTE  4)
(define XHCI-USBSTS-HCH   1)
(define XHCI-USBSTS-CNR   #x800)

(define XHCI-IMAN-IP 1)
(define XHCI-IMAN-IE 2)

(define XHCI-PORTSC-CCS #x1)
(define XHCI-PORTSC-PED #x2)
(define XHCI-PORTSC-PR  #x10)
(define XHCI-PORTSC-PP  #x200)
(define XHCI-PORTSC-CSC #x20000)
(define XHCI-PORTSC-PRC #x200000)
(define XHCI-PORTSC-SPEED-SHIFT 10)
(define XHCI-PORTSC-SPEED-MASK  #xF)

;; ---- runtime registers (offset from `rt`); interrupter 0 at +0x20 ----
(define XHCI-RT-IR0    #x20)
(define XHCI-IR-IMAN   #x00)
(define XHCI-IR-IMOD   #x04)
(define XHCI-IR-ERSTSZ #x08)
(define XHCI-IR-ERSTBA #x10)   ; 64-bit
(define XHCI-IR-ERDP   #x18)   ; 64-bit

;; ---- TRB types ----
(define TRB-NORMAL 1)  (define TRB-SETUP 2)   (define TRB-DATA 3)
(define TRB-STATUS 4)  (define TRB-LINK 6)
(define TRB-ENABLE-SLOT 9)        (define TRB-DISABLE-SLOT 10)
(define TRB-ADDRESS-DEVICE 11)    (define TRB-CONFIGURE-ENDPOINT 12)
(define TRB-EVENT-TRANSFER 32)    (define TRB-EVENT-CMD-COMPLETE 33)
(define TRB-EVENT-PORT-STATUS 34)
(define XHCI-CC-SUCCESS 1)        (define XHCI-CC-SHORT-PACKET 13)
(define XHCI-TRB-CYCLE 1)

;; endpoint context EP types
(define EP-TYPE-CONTROL 4) (define EP-TYPE-BULK-OUT 2) (define EP-TYPE-BULK-IN 6)
(define EP-TYPE-INTR-OUT 3) (define EP-TYPE-INTR-IN 7)

(define XHCI-RING-SIZE 64)
(define XHCI-BOUNCE-MAX 2048)

(define (trb-set-type t) (arithmetic-shift (bitwise-and t #x3F) 10))
(define (trb-type ctrl)  (bitwise-and (arithmetic-shift ctrl -10) #x3F))
(define (trb-cc status)  (bitwise-and (arithmetic-shift status -24) #xFF))
(define (trb-slot ctrl)  (bitwise-and (arithmetic-shift ctrl -24) #xFF))

;; ---- MMIO 32/64-bit access at absolute offset in the mapped window ----
(define (rd32 m off)   (bytes-u32-ref m off))
(define (wr32 m off v) (bytes-u32-set! m off v))
(define (rd64 m off)   (bytes-u64-ref m off))
(define (wr64 m off v) (bytes-u64-set! m off v))

;; ---- producer TRB ring: (buf phys ctrl) where ctrl is a 16-byte cell holding
;; enqueue (u32@0) and cycle (u32@4). One DMA page; 64 TRBs incl a Link at slot 63.
(define (ring-make)
  (let ((buf (dma-alloc-32 4096)) (ctrl (make-bytes 16)))
    (let ((phys (bytes-phys buf)))
      (bytes-u32-set! ctrl 0 0)    ; enqueue
      (bytes-u32-set! ctrl 4 1)    ; cycle
      (let ((o (* (- XHCI-RING-SIZE 1) 16)))   ; Link TRB -> ring base, Toggle Cycle
        (bytes-u64-set! buf o phys)
        (bytes-u32-set! buf (+ o 8) 0)
        (bytes-u32-set! buf (+ o 12) (bitwise-or (trb-set-type TRB-LINK) 2)))
      (list buf phys ctrl))))
(define (ring-buf r)  (car r))
(define (ring-phys r) (cadr r))
(define (ring-enq r)  (bytes-u32-ref (caddr r) 0))
(define (ring-cyc r)  (bytes-u32-ref (caddr r) 4))

;; Push a TRB; returns its physical address. Stamps the producer cycle bit and
;; handles the Link/wrap (set Link cycle, wrap enqueue, toggle producer cycle).
(define (ring-push r param status control)
  (let* ((buf (ring-buf r)) (enq (ring-enq r)) (cyc (ring-cyc r))
         (o (* enq 16)) (tphys (+ (ring-phys r) o)))
    (bytes-u64-set! buf o param)
    (bytes-u32-set! buf (+ o 8) status)
    (bytes-u32-set! buf (+ o 12)
                    (bitwise-or (bitwise-and control (bitwise-not XHCI-TRB-CYCLE))
                                (if (= cyc 1) XHCI-TRB-CYCLE 0)))
    (let ((nenq (+ enq 1)))
      (if (= nenq (- XHCI-RING-SIZE 1))
          (let ((lo (* (- XHCI-RING-SIZE 1) 16)))
            (bytes-u32-set! buf (+ lo 12)
                            (bitwise-or (trb-set-type TRB-LINK) 2 (if (= cyc 1) XHCI-TRB-CYCLE 0)))
            (bytes-u32-set! (caddr r) 0 0)                       ; enqueue = 0
            (bytes-u32-set! (caddr r) 4 (if (= cyc 1) 0 1)))     ; toggle cycle
          (bytes-u32-set! (caddr r) 0 nenq)))
    tphys))
