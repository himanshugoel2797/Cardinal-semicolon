;; ahci/port.clp -- per-port register layer + DMA region layout + bring-up.
;;
;; A single port (we drive port 0, slot 0 -- one command in flight, which is all
;; the block protocol below issues) needs three DMA regions, allocated as one
;; buffer each:
;;
;;   command list  : 32 command headers * 32 bytes = 1KB. Header 0 points at the
;;                   command table; we only ever use slot 0.
;;   received FIS  : 256 bytes, the HBA writes D2H/PIO/DMA-setup FISes here.
;;   command table : the H2D command FIS at offset 0 (up to 64 bytes), then the
;;                   PRDT starting at offset 0x80. PRDT entry 0 = DBA/DBAU/rsv/DBC,
;;                   where DBC = byte-count-1 with bit31 = interrupt-on-completion.
;;
;; The data buffer (the sector payload) is a fourth DMA region the caller owns.
;; Per-port register offsets are 0x100 + 0x80*port + field.

;; --- per-port register access -----------------------------------------------
(define (px-off port field) (+ #x100 (* #x80 port) field))
(define (px-rd abar port field)   (rd32 abar (px-off port field)))
(define (px-wr abar port field v) (wr32 abar (px-off port field) v))

;; Per-port register field offsets (relative to the port register block).
(define PxCLB  #x00)   ; command list base (lo)
(define PxCLBU #x04)   ; command list base (hi)
(define PxFB   #x08)   ; received-FIS base (lo)
(define PxFBU  #x0C)   ; received-FIS base (hi)
(define PxIS   #x10)   ; interrupt status (write 1 to clear)
(define PxIE   #x14)   ; interrupt enable
(define PxCMD  #x18)   ; command + status
(define PxTFD  #x20)   ; task file data (BSY/DRQ/ERR live here)
(define PxSIG  #x24)   ; signature
(define PxSSTS #x28)   ; SATA status (DET/SPD/IPM)
(define PxSCTL #x2C)   ; SATA control
(define PxSERR #x30)   ; SATA error (write 1s to clear)
(define PxSACT #x34)   ; SATA active (NCQ; unused here)
(define PxCI   #x38)   ; command issue (bit per slot)

;; PxCMD bits.
(define PxCMD-ST  #x00000001)   ; start (process the command list)
(define PxCMD-FRE #x00000010)   ; FIS receive enable
(define PxCMD-FR  #x00004000)   ; FIS receive running
(define PxCMD-CR  #x00008000)   ; command list running

;; PxTFD status bits.
(define PxTFD-ERR #x00000001)
(define PxTFD-DRQ #x00000008)
(define PxTFD-BSY #x00000080)

;; PxIE/PxIS: arm the completion-relevant sources (D2H reg, PIO setup, DMA setup,
;; plus the error bits). 0x7D8000FD is "all useful" without the reserved bits; we
;; just enable the common completion sources and a broad error mask.
(define PxIE-MASK #x7DC000FF)

;; --- DMA region layout ------------------------------------------------------
;; One command-list buffer (1KB), one received-FIS buffer (256B), one command-table
;; buffer (>=0x80 + PRDT). Returned as a list the issue path threads through.
(define CMDLIST-SIZE 1024)
(define RXFIS-SIZE   256)
(define CMDTBL-SIZE  512)    ; FIS (0..0x7F) + PRDT entries from 0x80 (1 entry used)
(define FIS-OFF      #x00)
(define PRDT-OFF     #x80)

;; Allocate the three regions with the right addressing width. s64a? picks
;; dma-alloc (full 64-bit phys) vs dma-alloc-32 (phys < 4GB, upper dwords 0).
(define (port-alloc-regions s64a?)
  (let ((alloc (if s64a? dma-alloc dma-alloc-32)))
    (list (alloc CMDLIST-SIZE)     ; command list
          (alloc RXFIS-SIZE)       ; received FIS
          (alloc CMDTBL-SIZE))))   ; command table

(define (regs-cmdlist r) (nth r 0))
(define (regs-rxfis r)   (nth r 1))
(define (regs-cmdtbl r)  (nth r 2))

;; Command-header slot 0 (32 bytes at command-list offset 0):
;;   dword0: bits0-4 = command-FIS length in dwords (CFL); bit6 = write (W);
;;           bits16-31 = PRDT length (PRDTL, number of entries).
;;   dword1: PRD byte count (PRDBC, HBA-written -- leave 0).
;;   dword2: command-table base (lo); dword3: base (hi).
;; cfl-dwords = FIS length / 4 (a 20-byte H2D FIS -> 5). write? sets bit6.
(define (cmdhdr-set! cmdlist ctbl-phys cfl-dwords write? prdtl)
  (let ((d0 (bitwise-or (bitwise-and cfl-dwords #x1F)
                        (if write? #x40 0)
                        (arithmetic-shift (bitwise-and prdtl #xFFFF) 16))))
    (bytes-u32-set! cmdlist 0 d0)
    (bytes-u32-set! cmdlist 4 0)                                     ; PRDBC
    (bytes-u32-set! cmdlist 8 (bitwise-and ctbl-phys #xFFFFFFFF))    ; CTBA
    (bytes-u32-set! cmdlist 12 (arithmetic-shift ctbl-phys -32))))   ; CTBAU

;; --- bring a port up --------------------------------------------------------
;; Idle the port (clear ST + FRE, wait for CR + FR to clear), wait for the link
;; (PxSSTS DET=3 = device present + PHY communication established), clear SERR + IS,
;; wait for the device to be ready (BSY|DRQ clear in PxTFD), program CLB/FB (with
;; the upper dwords from s64a), then FRE, then ST. Returns #t on success.
(define (port-idle! abar port)
  (px-wr abar port PxCMD
         (bitwise-and (px-rd abar port PxCMD)
                      (bitwise-not (bitwise-or PxCMD-ST PxCMD-FRE))))
  (wait-until (lambda ()
                (= 0 (bitwise-and (px-rd abar port PxCMD)
                                  (bitwise-or PxCMD-CR PxCMD-FR))))
              1000000000))

(define (port-link-up? abar port)
  (wait-until (lambda ()
                (= 3 (bitwise-and (px-rd abar port PxSSTS) #xF)))
              1000000000))

(define (port-bringup! abar port regions s64a?)
  (let ((cmdlist (regs-cmdlist regions))
        (rxfis   (regs-rxfis regions))
        (cmdtbl  (regs-cmdtbl regions)))
    (port-idle! abar port)
    (if (not (port-link-up? abar port))
        #f
        (begin
          ;; Clear any latched SATA error + interrupt status before we start.
          (px-wr abar port PxSERR #xFFFFFFFF)
          (px-wr abar port PxIS   #xFFFFFFFF)
          ;; Program the command-list + received-FIS base addresses.
          (let ((cl-phys (bytes-phys cmdlist))
                (fb-phys (bytes-phys rxfis)))
            (px-wr abar port PxCLB  (bitwise-and cl-phys #xFFFFFFFF))
            (px-wr abar port PxCLBU (if s64a? (arithmetic-shift cl-phys -32) 0))
            (px-wr abar port PxFB   (bitwise-and fb-phys #xFFFFFFFF))
            (px-wr abar port PxFBU  (if s64a? (arithmetic-shift fb-phys -32) 0)))
          ;; FRE first (so the HBA can post the D2H FIS), then start the engine.
          (px-wr abar port PxCMD (bitwise-or (px-rd abar port PxCMD) PxCMD-FRE))
          (px-wr abar port PxCMD (bitwise-or (px-rd abar port PxCMD) PxCMD-ST))
          ;; Wait for the device to leave BSY/DRQ (it settles after link-up).
          (wait-until (lambda ()
                        (= 0 (bitwise-and (px-rd abar port PxTFD)
                                          (bitwise-or PxTFD-BSY PxTFD-DRQ))))
                      2000000000)))))
