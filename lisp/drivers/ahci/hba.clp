;; ahci/hba.clp -- HBA (host bus adapter) global register layer.
;;
;; The ABAR (AHCI base address register = BAR5) is mapped as a single byte buffer;
;; every HBA + per-port register is a 32-bit dword at a fixed offset into it. This
;; part owns the global bring-up: read CAP (64-bit-addressing + port count), do the
;; BIOS/OS handoff (only when CAP2.BOH says the handshake exists), reset the HBA via
;; GHC.HR, re-enable AHCI mode (GHC.AE), and scan PI for implemented ports. The
;; per-port command machinery lives in the `port` part.
;;
;; All waits here YIELD (wait-until -> sleep) -- this runs inside a spawned context,
;; never a cli() boot path, so a stuck reset descheds rather than busy-spins.

;; --- ABAR register access ---------------------------------------------------
;; rd32/wr32 read/write a 32-bit HBA register; the ABAR buffer is little-endian
;; native, matching the HBA's dword registers.
(define (rd32 abar off)     (bytes-u32-ref abar off))
(define (wr32 abar off v)   (bytes-u32-set! abar off v))

;; Global HBA register offsets.
(define HBA-CAP   #x00)   ; host capabilities
(define HBA-GHC   #x04)   ; global host control
(define HBA-IS    #x08)   ; interrupt status (port bitmap)
(define HBA-PI    #x0C)   ; ports implemented (port bitmap)
(define HBA-CAP2  #x24)   ; host capabilities extended
(define HBA-BOHC  #x28)   ; BIOS/OS handoff control + status

;; GHC bits.
(define GHC-HR  #x00000001)   ; HBA reset
(define GHC-IE  #x00000002)   ; global interrupt enable
(define GHC-AE  #x80000000)   ; AHCI enable

;; CAP / CAP2 bits.
(define CAP-S64A #x80000000)  ; supports 64-bit addressing
(define CAP2-BOH #x00000001)  ; BIOS/OS handoff supported

;; BOHC bits.
(define BOHC-BOS #x00000001)  ; BIOS owned
(define BOHC-OOS #x00000002)  ; OS owned (we request this)
(define BOHC-BB  #x00000010)  ; BIOS busy

;; Does the HBA support 64-bit DMA addresses? (decides dma-alloc vs dma-alloc-32).
(define (hba-s64a? abar)
  (not (= 0 (bitwise-and (rd32 abar HBA-CAP) CAP-S64A))))

;; --- BIOS/OS handoff --------------------------------------------------------
;; Gated on CAP2.BOH: many HBAs (the QEMU ICH9 included) do NOT implement the
;; handoff, in which case BOHC is reserved and must be left alone. When supported,
;; request OS ownership and wait for BIOS to relinquish (BOS clear, BB clear).
(define (hba-handoff! abar)
  (if (= 0 (bitwise-and (rd32 abar HBA-CAP2) CAP2-BOH))
      'no-handoff
      (begin
        (wr32 abar HBA-BOHC (bitwise-or (rd32 abar HBA-BOHC) BOHC-OOS))
        (wait-until (lambda ()
                      (= 0 (bitwise-and (rd32 abar HBA-BOHC) BOHC-BOS)))
                    1000000000)              ; up to 1s for BIOS to release
        (wait-until (lambda ()
                      (= 0 (bitwise-and (rd32 abar HBA-BOHC) BOHC-BB)))
                    2000000000)              ; up to 2s for BIOS-busy to clear
        'handed-off)))

;; --- HBA reset --------------------------------------------------------------
;; AHCI 10.4.3: set GHC.AE first (some HBAs require AHCI mode before HR), set GHC.HR,
;; then poll until HR self-clears (the HBA is reset and ready). Re-assert GHC.AE
;; afterwards (HR clears it). Returns #t on a clean reset, #f on timeout.
(define (hba-reset! abar)
  (wr32 abar HBA-GHC (bitwise-or (rd32 abar HBA-GHC) GHC-AE))
  (wr32 abar HBA-GHC (bitwise-or (rd32 abar HBA-GHC) GHC-HR))
  (let ((ok (wait-until (lambda ()
                          (= 0 (bitwise-and (rd32 abar HBA-GHC) GHC-HR)))
                        1000000000)))        ; HR self-clears within 1s
    (wr32 abar HBA-GHC (bitwise-or (rd32 abar HBA-GHC) GHC-AE))
    ok))

;; --- port scan --------------------------------------------------------------
;; Return the list of implemented port indices (0..31) from the PI bitmap.
(define (hba-ports abar)
  (let ((pi (rd32 abar HBA-PI)))
    (let loop ((i 0) (acc '()))
      (if (= i 32)
          (reverse acc)
          (loop (+ i 1)
                (if (= 0 (bitwise-and pi (arithmetic-shift 1 i)))
                    acc
                    (cons i acc)))))))
