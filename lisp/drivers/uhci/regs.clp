;; uhci/regs -- UHCI register map, port-I/O helpers, and the transfer-descriptor
;; (TD) / queue-head (QH) builders. Ported from drivers/uhci (uhci.h + main.c).
;;
;; UHCI is a port-I/O controller: its registers live at an I/O BAR (in/out
;; instructions, sys-io), and its schedule is DMA memory the controller walks --
;; a 1024-entry frame list, every slot pointing at one persistent control QH whose
;; element pointer we swing onto a freshly built TD chain per transfer.

;; ---- I/O register offsets (relative to the I/O BAR) -------------------------
(define USBCMD-REG     #x00)   ; u16
(define USBSTS-REG     #x02)   ; u16
(define USBINTR-REG    #x04)   ; u16
(define FRNUM-REG      #x06)   ; u16
(define FRBASEADDR-REG #x08)   ; u32
(define SOFMOD-REG     #x0C)   ; u8
(define (PORTSC-REG x) (+ #x10 (* x 2)))   ; u16

(define USBCMD-RS      1)
(define USBCMD-HCRESET 2)
(define USBCMD-GRESET  4)
(define USBSTS-USBINT  1)

(define PORTSC-CURCONNECT 1)
(define PORTSC-CONNECTCHG 2)
(define PORTSC-PORTEN     4)
(define PORTSC-PORTENCHG  8)
(define PORTSC-LOWSPEED   #x100)
(define PORTSC-PORTRESET  #x200)

;; UHCI packet IDs.
(define UHCI-PID-SETUP #x2D)
(define UHCI-PID-IN    #x69)
(define UHCI-PID-OUT   #xE1)

(define PORT-COUNT  2)
(define FRAME-COUNT 1024)

;; Per-controller DMA scratch layout inside one 4 KiB 32-bit page.
(define UHCI-QH-OFF    #x000)
(define UHCI-TD-OFF    #x100)
(define UHCI-TD-COUNT  64)
(define UHCI-SETUP-OFF #x600)
(define UHCI-DATA-OFF  #x800)
(define UHCI-DATA-MAX  2048)

;; ---- port-I/O register access (iobar is the I/O BAR base) -------------------
(define (u-r16 iobar off)   (in-u16 (+ iobar off)))
(define (u-w16 iobar off v) (out-u16 (+ iobar off) v))
(define (u-w32 iobar off v) (out-u32 (+ iobar off) v))

;; ---- TD field encoders (a TD is 16 bytes; 4 little-endian dwords) -----------
;; status dword: Active(0x80) in the 8-bit status field at bits[23:16],
;; err_count=3 at bits[28:27], ls (low-speed) at bit26.
(define (td-status-dw ls)
  (bitwise-or #x00800000 (arithmetic-shift 3 27) (arithmetic-shift ls 26)))
;; Isochronous TD status dword: IOS (bit25) marks the TD isochronous (the HC clocks
;; it in its frame and never retries), Active(0x80) set. err_count stays 0 -- iso
;; has no error retries; ls stays 0 -- UHCI iso is full-speed only.
(define (td-iso-status-dw)
  (bitwise-or #x00800000 (arithmetic-shift 1 25)))
;; token dword: pid[7:0], device[14:8], endpoint[18:15], toggle[19], maxlen[31:21]
;; (maxlen encodes a length of N as N-1; 0x7FF = zero length).
(define (td-token-dw pid device endpoint toggle maxlen)
  (bitwise-or pid
              (arithmetic-shift device 8)
              (arithmetic-shift endpoint 15)
              (arithmetic-shift toggle 19)
              (arithmetic-shift (bitwise-and maxlen #x7FF) 21)))

;; Write TD `i` (link left 0 -- set by link-chain!) into the dma buffer.
(define (td! dma i status-dw token-dw bufptr)
  (let ((o (+ UHCI-TD-OFF (* i 16))))
    (bytes-u32-set! dma o 0)
    (bytes-u32-set! dma (+ o 4) status-dw)
    (bytes-u32-set! dma (+ o 8) token-dw)
    (bytes-u32-set! dma (+ o 12) bufptr)))

;; Link n TDs depth-first (bit2 = depth-first, pointing TD->TD); last terminates.
(define (link-chain! dma td-phys-base n)
  (let loop ((i 0))
    (if (= i n) 'done
        (begin
          (bytes-u32-set! dma (+ UHCI-TD-OFF (* i 16))
                          (if (= i (- n 1))
                              1                      ; Terminate
                              (bitwise-or (+ td-phys-base (* (+ i 1) 16)) 4)))
          (loop (+ i 1))))))

;; TD status reads: the 8-bit status field, the Active bit, fatal error bits
;; (STALL/DataBuffer/Babble/CRC-timeout/bitstuff -- NAK 0x08 excluded), act_len.
(define (td-st dma i)     (bit-extract (bytes-u32-ref dma (+ UHCI-TD-OFF (* i 16) 4)) 16 8))
(define (td-active? dma i) (not (= 0 (bitwise-and (td-st dma i) #x80))))
(define (td-fatal? dma i)  (not (= 0 (bitwise-and (td-st dma i) #x76))))
(define (td-actlen dma i)  (bit-extract (bytes-u32-ref dma (+ UHCI-TD-OFF (* i 16) 4)) 0 11))

;; The persistent control QH lives at offset 0: head terminates (single QH),
;; element points at the armed TD chain (or terminates when idle).
(define (qh-arm! dma td-phys-base)
  (bytes-u32-set! dma UHCI-QH-OFF 1)                 ; hlp = Terminate
  (bytes-u32-set! dma (+ UHCI-QH-OFF 4) td-phys-base)) ; elp -> first TD
(define (qh-idle! dma)
  (bytes-u32-set! dma (+ UHCI-QH-OFF 4) 1))          ; detach the chain

;; ---- isochronous scheduling --------------------------------------------------
;; Iso TDs live in their own DMA buffer (one per scheduled packet), referenced
;; DIRECTLY from frame-list slots rather than from behind the control QH, because
;; the HC must service each in its own frame. ISO-TD-COUNT caps a single
;; submission; ISO-DATA-MAX caps its total payload (kept under one page each).
(define ISO-TD-COUNT 64)
(define ISO-DATA-MAX 4096)

;; Write iso TD `i` into `itd` (its own 16-byte slot): link -> the control QH (so
;; control/bulk still run after the iso TD in that frame), iso status, token, buf.
(define (iso-td! itd i qh-phys token-dw bufptr)
  (let ((o (* i 16)))
    (bytes-u32-set! itd o (bitwise-or qh-phys 2))    ; link -> QH (Q=1)
    (bytes-u32-set! itd (+ o 4) (td-iso-status-dw))
    (bytes-u32-set! itd (+ o 8) token-dw)
    (bytes-u32-set! itd (+ o 12) bufptr)))
(define (iso-td-active? itd i) (not (= 0 (bitwise-and (bit-extract (bytes-u32-ref itd (+ (* i 16) 4)) 16 8) #x80))))
(define (iso-td-actlen itd i)  (bit-extract (bytes-u32-ref itd (+ (* i 16) 4)) 0 11))

;; The current frame index (FRNUM[10:0] indexes the 1024-entry frame list).
(define (uhci-frnum iobar) (bitwise-and (u-r16 iobar FRNUM-REG) #x3FF))
;; Point frame slot `f` at iso TD phys `p` (Q=0,T=0 -> the HC walks to the TD),
;; or restore it to the persistent control QH.
(define (frame-set-iso! fl f p) (bytes-u32-set! fl (* f 4) p))
(define (frame-set-qh!  fl f qh-phys) (bytes-u32-set! fl (* f 4) (bitwise-or qh-phys 2)))
