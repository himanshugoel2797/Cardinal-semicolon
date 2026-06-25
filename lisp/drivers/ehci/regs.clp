;; ehci/regs -- EHCI (USB 2.0) register map + the Queue Head / qTD builders.
;;
;; EHCI is MMIO: capability registers at the BAR base, operational registers at
;; base+CAPLENGTH. The schedule lives in DMA memory. We use the minimal
;; single-reusable-QH async model (mirroring uhci's single control QH): ONE Queue
;; Head is the async reclamation head; per transfer we build a qTD chain, rewrite
;; the QH's endpoint fields, and ARM it by writing the overlay's Next qTD Pointer
;; last. Control/bulk/interrupt all ride the async schedule -- the host just emits
;; the right token PIDs, so a one-shot async IN serves an interrupt poll exactly as
;; it serves bulk (the periodic schedule, needed only for hardware-paced iso/intr
;; intervals, is not used). Completion is polled from the qTD token in DMA (no MSI).

;; ---- capability registers (offset from MMIO base) ----------------------------
(define ECAP-CAPLENGTH #x00)   ; u8: operational-register offset
(define ECAP-HCSPARAMS #x04)   ; u32: N_PORTS in bits[3:0]
(define ECAP-HCCPARAMS #x08)   ; u32: 64-bit-addr cap, ext-caps pointer

;; ---- operational registers (offset from `op` = base + CAPLENGTH) -------------
(define EOP-USBCMD    #x00)
(define EOP-USBSTS    #x04)
(define EOP-USBINTR   #x08)
(define EOP-FRINDEX   #x0C)
(define EOP-CTRLDSSEG #x10)
(define EOP-PERIODICBASE #x14)
(define EOP-ASYNCBASE #x18)
(define EOP-CONFIGFLAG #x40)
(define (EOP-PORTSC n) (+ #x44 (* n 4)))

(define USBCMD-RS      #x1)        ; Run/Stop
(define USBCMD-HCRESET #x2)
(define USBCMD-ASE     #x20)       ; Async Schedule Enable
(define USBCMD-ITC-1   #x10000)    ; Interrupt Threshold Control = 1 microframe

(define USBSTS-HCHALTED #x1000)
(define USBSTS-ASS      #x8000)    ; Async Schedule Status (follows ASE)

(define PORTSC-CCS  #x1)           ; current connect status
(define PORTSC-CSC  #x2)           ; connect status change (W1C)
(define PORTSC-PED  #x4)           ; port enabled/disabled (RW; reset enables HS)
(define PORTSC-PEDC #x8)           ; port enable change (W1C)
(define PORTSC-PR   #x100)         ; port reset
(define PORTSC-LINE-SHIFT 10)      ; line status bits[11:10]; 01 = K-state = low speed
(define PORTSC-PP   #x1000)        ; port power
(define PORTSC-PO   #x2000)        ; port owner (1 -> hand to companion controller)
;; Bits to preserve when W1C-ing the change bits. PED is deliberately EXCLUDED:
;; it is write-1-to-disable (not a value to write back), so OR-ing a read-back PED=1
;; into the ack write would disable the just-enumerated port. Only PP/PO are RW and
;; safe to carry through; the change bits themselves are written as 1 to clear.
(define PORTSC-PRESERVE (bitwise-or PORTSC-PP PORTSC-PO))

;; ---- MMIO access -------------------------------------------------------------
(define (e-r32 m off)   (bytes-u32-ref m off))
(define (e-w32 m off v) (bytes-u32-set! m off v))

;; ---- DMA schedule layout (one 4 KiB page) ------------------------------------
;; QH at 0 (48 bytes, 32-aligned), qTDs at 0x80 (32 bytes each), the 8-byte SETUP
;; staging buffer at 0x300. Bulk/control data uses a SEPARATE page-aligned buffer.
(define EHCI-QH-OFF    #x000)
(define EHCI-QTD-OFF   #x080)      ; qTD i at 0x80 + i*32
(define EHCI-SETUP-OFF #x300)
(define EHCI-DATA-MAX  4096)       ; per-transfer data cap (single qTD, page + cross)

;; qTD PIDs (token bits[9:8]).
(define EHCI-PID-OUT   0)
(define EHCI-PID-IN    1)
(define EHCI-PID-SETUP 2)

;; ---- qTD (32 bytes: next, alt-next, token, 5 buffer pointers) ----------------
;; Build qTD `i`. next-phys is the next qTD (or 1 = Terminate). buf0 is the data
;; pointer (page | offset); buf1 is the next page, derived from buf0, so a transfer
;; that crosses one page boundary still resolves (our buffers are page-aligned, so
;; a <=4 KiB transfer stays in page 0 and buf1 is unused but valid).
(define (qtd! sched i next-phys pid nbytes toggle buf0 ioc)
  (let ((o (+ EHCI-QTD-OFF (* i 32)))
        (buf1 (+ (bitwise-and buf0 (bitwise-not #xFFF)) #x1000)))
    (bytes-u32-set! sched o next-phys)               ; Next qTD Pointer
    (bytes-u32-set! sched (+ o 4) 1)                 ; Alternate Next = Terminate
    (bytes-u32-set! sched (+ o 8)
                    (bitwise-or #x80                              ; Active
                                (arithmetic-shift pid 8)          ; PID[9:8]
                                (arithmetic-shift 3 10)           ; CERR = 3
                                (arithmetic-shift (bitwise-and nbytes #x7FFF) 16)  ; Total Bytes
                                (if ioc #x8000 0)                 ; IOC
                                (arithmetic-shift toggle 31)))    ; Data Toggle
    (bytes-u32-set! sched (+ o 12) buf0)
    (bytes-u32-set! sched (+ o 16) buf1)
    (bytes-u32-set! sched (+ o 20) 0)
    (bytes-u32-set! sched (+ o 24) 0)
    (bytes-u32-set! sched (+ o 28) 0)))

(define (qtd-token sched i)     (bytes-u32-ref sched (+ EHCI-QTD-OFF (* i 32) 8)))
(define (qtd-active? sched i)   (not (= 0 (bitwise-and (qtd-token sched i) #x80))))
(define (qtd-remaining sched i) (bit-extract (qtd-token sched i) 16 15))  ; bytes NOT transferred

;; ---- Queue Head (48 bytes: link, ep-char, ep-cap, current-qTD, overlay) ------
;; The overlay (DW4..DW11) mirrors a qTD; the HC copies the Next qTD into it and
;; executes. Halted shows in the overlay token (DW6 at +0x18).
(define (qh-overlay-halted? sched)
  (not (= 0 (bitwise-and (bytes-u32-ref sched (+ EHCI-QH-OFF 24)) #x40))))

;; Initialize the QH as an idle async reclamation head: horizontal link -> itself
;; (QH type, T=0), H=1, no active qTD. Done once at bring-up, before ASE.
(define (qh-init-head! sched qh-phys)
  (bytes-u32-set! sched EHCI-QH-OFF (bitwise-or qh-phys 2))      ; link -> self, typ=QH
  (bytes-u32-set! sched (+ EHCI-QH-OFF 4) (arithmetic-shift 1 15)) ; H=1, addr 0
  (bytes-u32-set! sched (+ EHCI-QH-OFF 8) (arithmetic-shift 1 30)) ; mult=1
  (bytes-u32-set! sched (+ EHCI-QH-OFF 12) 0)
  (bytes-u32-set! sched (+ EHCI-QH-OFF 16) 1)                    ; overlay next = T
  (bytes-u32-set! sched (+ EHCI-QH-OFF 20) 1)                    ; overlay alt = T
  (bytes-u32-set! sched (+ EHCI-QH-OFF 24) 0))                   ; overlay token inactive

;; Reconfigure the (idle) QH for a transfer and ARM it. eps: 0=full 1=low 2=high.
;; DTC=1 -> the QH takes the data toggle from each qTD (we set toggles explicitly).
;; The non-HS control flag (C) is needed for a full/low-speed control endpoint. The
;; overlay's Next qTD Pointer (DW4) is written LAST: that single dword is the atomic
;; arm, so the HC never executes a half-updated QH while the async schedule runs.
(define (qh-config! sched first-qtd-phys addr ep eps mps is-control)
  (let ((o EHCI-QH-OFF))
    (bytes-u32-set! sched (+ o 4)
                    (bitwise-or (bitwise-and addr #x7F)
                                (arithmetic-shift (bitwise-and ep #xF) 8)
                                (arithmetic-shift eps 12)            ; EPS[13:12]
                                (arithmetic-shift 1 14)              ; DTC = 1
                                (arithmetic-shift 1 15)              ; H = 1
                                (arithmetic-shift (bitwise-and mps #x7FF) 16)
                                (if (and is-control (not (= eps 2))) (arithmetic-shift 1 27) 0) ; C
                                (arithmetic-shift 4 28)))            ; RL = 4 (NAK reload)
    (bytes-u32-set! sched (+ o 8) (arithmetic-shift 1 30))          ; mult = 1, no split
    (bytes-u32-set! sched (+ o 12) 0)                               ; current qTD
    (bytes-u32-set! sched (+ o 20) 1)                               ; overlay alt = T
    (bytes-u32-set! sched (+ o 24) 0)                               ; overlay token inactive
    (bytes-u32-set! sched (+ o 28) 0) (bytes-u32-set! sched (+ o 32) 0)
    (bytes-u32-set! sched (+ o 36) 0) (bytes-u32-set! sched (+ o 40) 0)
    (bytes-u32-set! sched (+ o 44) 0)
    (bytes-u32-set! sched (+ o 16) first-qtd-phys)))               ; DW4 next qTD -- ARM

;; Idle the QH after a transfer: overlay next = Terminate, token inactive.
(define (qh-idle! sched)
  (bytes-u32-set! sched (+ EHCI-QH-OFF 16) 1)
  (bytes-u32-set! sched (+ EHCI-QH-OFF 24) 0))
