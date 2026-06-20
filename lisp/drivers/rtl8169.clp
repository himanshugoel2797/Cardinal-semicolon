;; rtl8169: the Realtek RTL8168/8111 gigabit NIC family, in Cardinal Lisp.
;;
;; A port of the C driver (drivers/rtl8169/). Unlike the 8139, the 8168 is a
;; "C+"-mode part: it DMAs 16-byte ring DESCRIPTORS (not a flat circular byte
;; ring), uses 64-bit buffer addresses, and signals via MSI-X rather than a
;; legacy INTx line. So this is structurally the virtio-net shape -- a descriptor
;; ring + an MSI-driven RX pump -- not the 8139's poll-the-flat-ring shape.
;;
;; CAVEAT (real-hardware only): QEMU does NOT emulate an 8168/8111; there is no
;; `-device rtl8168`. This driver cannot be smoke-tested in CI -- it is exercised
;; only on real RTL8111G silicon. To keep SOMETHING testable without the device,
;; the pure descriptor build/parse and the MAC read are factored into top-level
;; functions (tx-desc-build! / rx-desc-consume / rx-desc-init! / read-mac) that a
;; self-test can drive over mock byte buffers, exactly like rtl8139.clp's
;; rx-parse-one / tx-fill!.
;;
;; Like virtio-net and rtl8139 this driver is pure transport: it registers with
;; the corenetwork service (handing it the MAC + a TX context) and forwards every
;; received frame to it; all ethernet/ARP/IP framing lives in the service.
;;
;; Capabilities: sys-mmio (mmio-map + dma-alloc + bytes-phys), sys-pci (pci-find +
;; pci-assign-bars + pci-setup-msi + the MSI wake bridge msi-count/msi-wait), and
;; the generic driver-util helpers. The 8168 DMAs 64-bit addresses, so dma-alloc
;; (not dma-alloc-32) is fine. sys-irq is NOT needed -- RX is MSI-driven.
(define-module rtl8169
  (export rtl8169-init read-mac tx-desc-build! rx-desc-init! rx-desc-consume)
  (import sys-mmio sys-pci driver-util)

;; --- register map (MMIO BAR2, mapped over a #x100 window) --------------------

(define IDR0       #x00)   ; 6-byte MAC (read after reset)
(define TX-ADDR    #x20)   ; u32 lo @0x20, u32 hi @0x24: tx descriptor ring phys
(define CMD-REG    #x37)   ; u8 command
(define TPPOLL     #x38)   ; u8 transmit-priority poll: kick a TX queue
(define IMR        #x3C)   ; u16 interrupt mask
(define ISR-REG    #x3E)   ; u16 interrupt status (write-1-to-clear)
(define TX-CFG     #x40)   ; u32 transmit config (also carries hwrev in high bits)
(define RCR        #x44)   ; u32 receive config
(define _93C56-CMD #x50)   ; u8 eeprom/config-area lock
(define CONFIG1    #x52)   ; u8
(define PHYAR      #x60)   ; u32 indirect MII (PHY) access
(define MAX-RX     #xDA)   ; u16 max rx packet size
(define RX-ADDR    #xE4)   ; u32 lo @0xE4, u32 hi @0xE8: rx descriptor ring phys
(define CPLUS-CMD  #xE0)   ; u16 "C+" command register
(define MAX-TX     #xEC)   ; u8  max tx packet size (units of 128 bytes)
(define MISC-REG   #xF0)   ; u32 misc (clear bit 0x00080000 = RXDV-gate)

;; CMD (CMD-REG) bits.
(define CMD-RST  (arithmetic-shift 1 4))
(define CMD-RXEN (arithmetic-shift 1 3))
(define CMD-TXEN (arithmetic-shift 1 2))

;; TPPOLL: poll the Normal-Priority (gigE) TX queue. On the 8168 a queued TX
;; descriptor is not fetched until this is written -- otherwise the frame sits
;; with OWN=1 forever (received fine, never transmitted).
(define TPPOLL-NPQ #x40)

;; PHYAR (0x60): (reg<<16) | (data & 0xFFFF) | BUSY; BUSY clears on write-done.
(define PHYAR-BUSY #x80000000)
(define PHYAR-DATA #x0000FFFF)

;; Interrupt causes: ROK (rx ok) / TOK (tx ok).
(define INTR-ROK (arithmetic-shift 1 0))
(define INTR-TOK (arithmetic-shift 1 2))

;; C+ command (0xE0) 8168G value = PCI_MRW|RXCSUM|MACSTAT_DIS|0x0001. Bit 0 here
;; is NOT the legacy TX-enable; on the MAC-statistics (8168G) path it is an
;; implied magic bit the BSD `re` drivers set alongside MACSTAT_DIS.
(define CPLUS-8168G
  (bitwise-or #x0008 #x0020 #x0080 #x0001))

;; RCR base (BSD RL_RXCFG_CONFIG): FIFO threshold none (7<<13) | bufsz/EarlyOff-V2
;; (3<<11) | RX max DMA unlimited (7<<8); then OR the low 6 bits (#x3F) to accept
;; all packet types (allphys/match/multi/broad/runt/err).
(define RCR-VALUE
  (bitwise-or (arithmetic-shift 7 13)
              (arithmetic-shift 3 11)
              (arithmetic-shift 7 8)
              #x3F))

;; TX config: max transmit rate (3<<24) | unlimited burst (7<<8).
(define TXCFG-VALUE
  (bitwise-or (arithmetic-shift 3 24) (arithmetic-shift 7 8)))

;; Geometry. The C driver uses 1024 descriptors x 2048-byte packets (~4MB/dir);
;; that is excessive for our traffic, so SHRINK to 32 descriptors x 2048 bytes
;; (~64KiB of buffers + 512B of descriptors per direction). One contiguous DMA
;; buffer per direction holds NRX/NTX slots of PKT-SIZE bytes; descriptor i points
;; at slot i, so the completed frame for descriptor i is at offset i*PKT-SIZE.
(define NRX 32)
(define NTX 32)
(define PKT-SIZE 2048)
(define DESC-SIZE 16)              ; each ring descriptor is 16 bytes (4 dwords)

;; Descriptor dword0 control bits (shared layout for the bits we set).
(define DESC-OWN (arithmetic-shift 1 31))
(define DESC-EOR (arithmetic-shift 1 30))   ; end-of-ring marker on the last desc
(define TX-FS    (arithmetic-shift 1 29))   ; first segment (TX) -- dword0 bit 29
(define TX-LS    (arithmetic-shift 1 28))   ; last segment  (TX) -- dword0 bit 28
(define RX-LEN-MASK #x3FFF)                 ; rx frame_length is dword0 bits[13:0]

;; --- descriptor ring layout (pure helpers, hardware-free) --------------------
;; A descriptor is 4 little-endian dwords at byte offset i*DESC-SIZE in the ring
;; buffer:
;;   dword0 = control/status (OWN, EOR, FS/LS, frame_length)
;;   dword1 = reserved (0)
;;   dword2 = buffer phys low 32
;;   dword3 = buffer phys high 32
;; These set/parse helpers take the ring buffer + descriptor index, so a self-test
;; can drive them over a plain make-bytes buffer with mock phys addresses.

(define (desc-off i) (* i DESC-SIZE))

;; Write a descriptor's buffer address (dword2/3) and dword0 control word.
(define (desc-write! ring i ctl buf-phys)
  (let ((o (desc-off i)))
    (bytes-u32-set! ring (+ o 4) 0)                                   ; rsvd
    (bytes-u32-set! ring (+ o 8) (bitwise-and buf-phys #xFFFFFFFF))   ; buf lo
    (bytes-u32-set! ring (+ o 12)
                    (bitwise-and (arithmetic-shift buf-phys -32) #xFFFFFFFF)) ; buf hi
    (bytes-u32-set! ring o ctl)))                                     ; dword0 LAST

;; Initialise RX descriptor i: device-owned (OWN=1), frame_length preset to the
;; buffer size, EOR on the last. buf-phys is slot i's buffer physical address.
(define (rx-desc-init! ring i buf-phys last?)
  (desc-write! ring i
    (bitwise-or DESC-OWN
                (if last? DESC-EOR 0)
                (bitwise-and PKT-SIZE RX-LEN-MASK))
    buf-phys))

;; Initialise TX descriptor i: driver-owned (OWN=0), no length yet, EOR on the
;; last. The frame_length + OWN are written per-transmit by tx-desc-build!.
(define (tx-desc-init! ring i buf-phys last?)
  (desc-write! ring i (if last? DESC-EOR 0) buf-phys))

;; Parse an RX descriptor: #f if still NIC-owned (OWN=1, empty), else the frame
;; length (dword0 bits[13:0]). Pure -- a self-test pre-stamps dword0 and checks.
(define (rx-desc-consume ring i)
  (let ((d0 (bytes-u32-ref ring (desc-off i))))
    (if (= 0 (bitwise-and d0 DESC-OWN))
        (bitwise-and d0 RX-LEN-MASK)      ; OWN==0 => NIC filled it
        #f)))                              ; still owned by the NIC: empty

;; Re-arm RX descriptor i after consuming it: dword0 = (PKT-SIZE & 0x3FFF) | OWN
;; | (EOR if last). Hands the buffer back to the NIC.
(define (rx-desc-rearm! ring i last?)
  (bytes-u32-set! ring (desc-off i)
    (bitwise-or DESC-OWN
                (if last? DESC-EOR 0)
                (bitwise-and PKT-SIZE RX-LEN-MASK))))

;; Build (start) a TX on descriptor i: write dword0 = FS|LS|OWN|len. The buffer
;; address was set once at init; here we only stamp the length + hand it over.
;; Pure (ring + index + len), so a self-test asserts the control word bits.
(define (tx-desc-build! ring i len last?)
  (bytes-u32-set! ring (desc-off i)
    (bitwise-or DESC-OWN TX-FS TX-LS
                (if last? DESC-EOR 0)
                (bitwise-and len #xFFFF))))

;; TX done? OWN clears (via DMA) when the NIC finishes the previous send.
(define (tx-desc-idle? ring i)
  (= 0 (bitwise-and (bytes-u32-ref ring (desc-off i)) DESC-OWN)))

;; --- TX: a round-robin over the NTX descriptors ------------------------------
;; Pure-ish core (ring + buffer + free-slot cell are args -> testable without
;; hardware): wait (bounded) for this slot's prior send to drain, copy the frame
;; into its buffer, stamp the descriptor, kick TPPOLL, advance the cell.

(define (tx-send! regs txring txbuf free-cell frame len)
  (if (or (> len PKT-SIZE) (<= len 0))
      #f
      (let ((i (cell-ref free-cell)))
        ;; Wait (yields between polls) for the NIC to release this descriptor.
        (wait-until (lambda () (tx-desc-idle? txring i)) 100000000)   ; 100ms
        (bytes-copy-into! txbuf (* i PKT-SIZE) frame len)
        (tx-desc-build! txring i len (= i (- NTX 1)))
        ;; Kick the Normal-Priority TX queue so the NIC fetches the descriptor.
        (bytes-u8-set! regs TPPOLL TPPOLL-NPQ)
        (cell-set! free-cell (modulo (+ i 1) NTX))
        'sent)))

;; --- RX pump -----------------------------------------------------------------
;; Sweep every RX descriptor; for each NIC-filled one, hand the frame (snapshotted
;; out of the recycled rxbuf) to `handler`, then re-arm the descriptor. The C ISR
;; does the same full sweep on each ROK; sweeping all of them is simplest and
;; cannot miss a wrapped completion.

(define (rx-sweep! rxring rxbuf handler)
  (let loop ((i 0))
    (if (= i NRX)
        'swept
        (let ((flen (rx-desc-consume rxring i)))
          (if flen
              (begin
                (handler (* i PKT-SIZE) flen)
                (rx-desc-rearm! rxring i (= i (- NRX 1)))))
          (loop (+ i 1))))))

;; --- PHY (MII) ---------------------------------------------------------------
;; Indirect MII write through PHYAR: write (reg<<16)|(data&0xFFFF)|BUSY, then poll
;; until the NIC clears BUSY. Bounded so a wedged PHY can't hang boot.

(define (phy-write regs reg data)
  (bytes-u32-set! regs PHYAR
    (bitwise-or (arithmetic-shift reg 16)
                (bitwise-and data PHYAR-DATA)
                PHYAR-BUSY))
  (wait-until
    (lambda () (= 0 (bitwise-and (bytes-u32-ref regs PHYAR) PHYAR-BUSY)))
    20000000)    ; 20ms
  ;; Post-access settle: the C driver (and BSD `re`) wait ~20us after BUSY clears
  ;; before the next MII cycle -- on real 8168G silicon a back-to-back access (as
  ;; phy-wake does) can otherwise arrive before the prior write takes effect and
  ;; leave the PHY powered down (no link). Harmless on QEMU, load-bearing on hw.
  (sleep 20000))   ; 20us in ns

;; Wake the PHY: select page 0, then clear power-down. Without this a powered-down
;; PHY never links. (RL_FLAG_PHYWAKE in the BSD drivers.)
(define (phy-wake regs)
  (phy-write regs #x1F 0)    ; select PHY page 0
  (phy-write regs #x0E 0))   ; clear power-down

;; --- PCI power management: bring the device to D0 -----------------------------
;; A RealTek NIC firmware never initialised typically sits in D3hot: config space
;; is readable (we see deviceID 0x8168) but its memory BARs are dead until a
;; D3->D0 transition. Walk the capability list (capabilitiesPtr @ 0x34; each cap:
;; id @ ptr, next @ ptr+1) to the Power-Management capability (id 0x01) and clear
;; the low 2 PowerState bits in PMCSR (ptr+4, u16). Spec allows up to 10ms to
;; settle. `cfg` is the mapped ECAM; reads/writes are config-space accesses.
(define PCI-CAP-PTR #x34)
(define PCI-CAP-PWM #x01)

(define (power-on-d0 cfg)
  (let loop ((ptr (bytes-u8-ref cfg PCI-CAP-PTR)) (guard 0))
    (if (or (= ptr 0) (> guard 48))
        'done
        (if (= (bytes-u8-ref cfg ptr) PCI-CAP-PWM)
            (let ((pmcsr (bytes-u16-ref cfg (+ ptr 4))))
              (if (not (= 0 (bitwise-and pmcsr 3)))
                  (begin
                    (display "[rtl8169] waking device D3->D0") (newline)
                    (bytes-u16-set! cfg (+ ptr 4) (bitwise-and pmcsr (bitwise-not 3)))
                    (sleep 10000000)))   ; 10ms settle
              'done)
            (loop (bytes-u8-ref cfg (+ ptr 1)) (+ guard 1))))))

;; --- bring-up ----------------------------------------------------------------

(define RTL-VID #x10EC)
(define RTL-DID #x8168)

;; Read the 6-byte MAC out of IDR0 (after reset). Pure over the regs buffer.
(define (read-mac regs)
  (list (bytes-u8-ref regs (+ IDR0 0)) (bytes-u8-ref regs (+ IDR0 1))
        (bytes-u8-ref regs (+ IDR0 2)) (bytes-u8-ref regs (+ IDR0 3))
        (bytes-u8-ref regs (+ IDR0 4)) (bytes-u8-ref regs (+ IDR0 5))))

;; Populate the RX ring (descriptors + buffers): descriptor i -> rxbuf slot i,
;; OWN=1, EOR on the last. Returns nothing useful; mutates rxring.
(define (rx-ring-init! rxring rxbuf)
  (let ((base (bytes-phys rxbuf)))
    (let loop ((i 0))
      (if (< i NRX)
          (begin
            (rx-desc-init! rxring i (+ base (* i PKT-SIZE)) (= i (- NRX 1)))
            (loop (+ i 1)))))))

;; Populate the TX ring: descriptor i -> txbuf slot i, OWN=0, EOR on the last.
(define (tx-ring-init! txring txbuf)
  (let ((base (bytes-phys txbuf)))
    (let loop ((i 0))
      (if (< i NTX)
          (begin
            (tx-desc-init! txring i (+ base (* i PKT-SIZE)) (= i (- NTX 1)))
            (loop (+ i 1)))))))

;; rtl8169-init takes the corenetwork service handle. It runs INSIDE a spawned
;; context (so wait-until/sleep yield). Discovery + config-space setup are
;; synchronous; the reset settle / PHY polls use wait-until (which yields). On any
;; failure it logs and returns #f. On success it spawns a TX context + an MSI RX
;; pump, registers with the network stack, and returns 'ok.
(define (rtl8169-init net)
  (let ((ecam (pci-find RTL-VID RTL-DID)))
    (if (not ecam)
        (begin (display "[rtl8169] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam #x1000)))
          ;; Enable memory-space decode + bus-mastering BEFORE any BAR/MMIO
          ;; access -- with mem-decode off every register reads back 0xFF and the
          ;; reset bit appears never to clear.
          (pci-enable-mem-bus-master! cfg)
          ;; Bring the NIC to D0 (BARs are dead in D3hot) before touching regs.
          (power-on-d0 cfg)
          ;; The register window is the 64-bit BAR2. Firmware usually assigns it;
          ;; if not, self-assign (opens the bridge path) and re-read BAR2.
          (let* ((b0 (bar-base cfg 2))
                 (base (if (= b0 0)
                           (begin (pci-assign-bars ecam) (bar-base cfg 2))
                           b0)))
            (if (= base 0)
                (begin (display "[rtl8169] no BAR2 register window") (newline) #f)
                (let ((regs (mmio-map base #x100)))
                  ;; Software reset, bounded so a wedged/absent NIC can't hang boot.
                  (bytes-u8-set! regs CMD-REG CMD-RST)
                  (if (not (wait-until
                             (lambda () (= 0 (bitwise-and (bytes-u8-ref regs CMD-REG) CMD-RST)))
                             100000000))
                      (begin (display "[rtl8169] reset timed out") (newline) #f)
                      (begin
                        ;; hwrev (TX-CFG high bits) is informational; this driver
                        ;; targets the 8168G/8111G path. Wake the PHY so link comes up.
                        (phy-wake regs)
                        ;; Allocate the descriptor rings + packet buffers (64-bit
                        ;; DMA is fine on the 8168, so plain dma-alloc).
                        (let ((rxring (dma-alloc (* NRX DESC-SIZE)))
                              (txring (dma-alloc (* NTX DESC-SIZE)))
                              (rxbuf  (dma-alloc (* NRX PKT-SIZE)))
                              (txbuf  (dma-alloc (* NTX PKT-SIZE))))
                          (if (or (not rxring) (not txring) (not rxbuf) (not txbuf))
                              (begin (display "[rtl8169] DMA buffer alloc failed") (newline) #f)
                              (begin
                                (rx-ring-init! rxring rxbuf)
                                (tx-ring-init! txring txbuf)
                                (let ((mac (read-mac regs)))
                                  ;; Configure the C+ register FIRST (BSD: "we must
                                  ;; configure the C+ register before all others").
                                  (bytes-u16-set! regs CPLUS-CMD CPLUS-8168G)
                                  ;; Unlock the config area, (nothing to poke), re-lock.
                                  (bytes-u8-set! regs _93C56-CMD #xC0)   ; unlock
                                  (bytes-u8-set! regs _93C56-CMD #x00)   ; lock
                                  ;; RX/TX config + max sizes + ring phys addrs.
                                  (bytes-u32-set! regs RCR RCR-VALUE)
                                  (bytes-u32-set! regs TX-CFG TXCFG-VALUE)
                                  ;; Zero the missed-packet counter (0x4C), as the C
                                  ;; init does -- statistics only, harmless to skip.
                                  (bytes-u32-set! regs #x4C 0)
                                  (bytes-u8-set!  regs MAX-TX (/ PKT-SIZE 128))
                                  (bytes-u32-set! regs RX-ADDR
                                    (bitwise-and (bytes-phys rxring) #xFFFFFFFF))
                                  (bytes-u32-set! regs (+ RX-ADDR 4)
                                    (bitwise-and (arithmetic-shift (bytes-phys rxring) -32) #xFFFFFFFF))
                                  (bytes-u32-set! regs TX-ADDR
                                    (bitwise-and (bytes-phys txring) #xFFFFFFFF))
                                  (bytes-u32-set! regs (+ TX-ADDR 4)
                                    (bitwise-and (arithmetic-shift (bytes-phys txring) -32) #xFFFFFFFF))
                                  ;; Clear the RXDV gate (MISC bit 0x00080000).
                                  (bytes-u32-set! regs MISC-REG
                                    (bitwise-and (bytes-u32-ref regs MISC-REG)
                                                 (bitwise-not #x00080000)))
                                  (bytes-u16-set! regs MAX-RX PKT-SIZE)
                                  ;; Mark driver loaded (CONFIG1 bit 0x20).
                                  (bytes-u8-set! regs CONFIG1
                                    (bitwise-or (bytes-u8-ref regs CONFIG1) #x20))
                                  ;; Set up MSI-X NOW (after reset -- the reset clears
                                  ;; the MSI-X table, which lives in the NIC's memory).
                                  (let ((msi (pci-setup-msi ecam)))
                                    (if (not msi)
                                        (begin (display "[rtl8169] MSI-X setup failed") (newline) #f)
                                        (begin
                                          ;; Undocumented 8168G chip-poke the C
                                          ;; driver applies just before enabling
                                          ;; RX/TX (memar[0xE2] |= 0x5100) -- a real
                                          ;; quirk in the C; kept for fidelity.
                                          (bytes-u16-set! regs #xE2
                                            (bitwise-or (bytes-u16-ref regs #xE2) #x5100))
                                          ;; Enable RX+TX.
                                          (bytes-u8-set! regs CMD-REG (bitwise-or CMD-RXEN CMD-TXEN))
                                          (let ((free-cell (make-cell 0)))
                                            ;; TX context: the service sends (tx frame len);
                                            ;; copy + post on the next descriptor.
                                            (let ((tx-ctx
                                                    (spawn-restricted '() (lambda ()
                                                      (let loop ()
                                                        (let ((m (recv)))
                                                          (if (eq? (car m) 'tx)
                                                              (tx-send! regs txring txbuf free-cell (cadr m) (caddr m)))
                                                          (loop)))))))
                                              ;; RX pump: park on the MSI; on each wake
                                              ;; mask IMR, ack ISR, sweep the ring
                                              ;; (forwarding each snapshotted frame), then
                                              ;; re-enable IMR -> re-arms edge MSI for
                                              ;; anything pending. (Follows the BSD `re`
                                              ;; mask/ack/service/unmask sequence.)
                                              (spawn-restricted '() (lambda ()
                                                (let loop ((seen (msi-count msi)))
                                                  (bytes-u16-set! regs IMR 0)
                                                  (bytes-u16-set! regs ISR-REG (bytes-u16-ref regs ISR-REG)) ; ack
                                                  (rx-sweep! rxring rxbuf
                                                    (lambda (off len)
                                                      (send net (list 'rx (copy-bytes rxbuf off len) len))))
                                                  (bytes-u16-set! regs IMR (bitwise-or INTR-ROK INTR-TOK))
                                                  (if (> (msi-count msi) seen)
                                                      (loop (msi-count msi))
                                                      (begin (msi-wait msi seen) (loop (msi-count msi)))))))
                                              ;; Announce to the stack: MAC + the TX context.
                                              (send net (list 'register-nic mac tx-ctx))
                                              ;; Enable the NIC's interrupt generation LAST --
                                              ;; the MSI cap is on, the pump exists, so an
                                              ;; edge-triggered MSI for an already-pending RX
                                              ;; is not lost.
                                              (bytes-u16-set! regs ISR-REG #xFFFF)   ; clear stale causes
                                              (bytes-u16-set! regs IMR (bitwise-or INTR-ROK INTR-TOK))
                                              (display "[rtl8169] registered with network stack") (newline)
                                              'ok))))))))))))))))))) ; last ) closes (define-module rtl8169 ...)
