;; e1000e: the Intel gigabit Ethernet family (82540 "e1000" + 82574L "e1000e"),
;; in Cardinal Lisp.
;;
;; Both the legacy 82540 (8086:100e) and the 82574L (8086:10d3) drive fine off
;; the LEGACY descriptor path in QEMU -- 16-byte RX/TX descriptors, a flat MMIO
;; register window in BAR0, and an MSI RX pump. (The igb-class adapters, e.g.
;; 8086:10c9, use the ADVANCED descriptor layout and a different ring-head model;
;; they are deferred -- bind only the two legacy IDs to this driver.)
;;
;; Structurally this is the virtio-net / rtl8169 shape: descriptor rings + an
;; MSI-driven RX pump, NOT the 8139's poll-the-flat-ring shape. Like the other
;; NIC drivers it is pure transport: it registers with the corenetwork service
;; (handing over the MAC + a TX context) and forwards every received frame to it;
;; all ethernet/ARP/IP framing lives in the service.
;;
;; The pure descriptor build/parse + the MAC read are factored into top-level
;; exports (tx-desc-build! / rx-desc-status / rx-desc-len / read-mac) so a host
;; self-test can drive them over mock byte buffers with no hardware present.
;;
;; Capabilities: sys-mmio (mmio-map + dma-alloc-32 + bytes-phys), sys-pci
;; (pci-assign-bars + pci-setup-msi + the MSI wake bridge msi-count/msi-wait),
;; and the generic driver-util helpers. The legacy descriptors carry 64-bit
;; buffer addresses, but to stay safe on the 82540 (whose firmware-less BARs we
;; place low) the rings + buffers are allocated with dma-alloc-32 (<4GiB phys).
(define-module e1000e
  (export e1000e-init read-mac
          tx-desc-build! tx-desc-done? rx-desc-status rx-desc-len rx-desc-eop?)
  (import sys-mmio sys-pci driver-util)

;; --- register map (MMIO BAR0) ------------------------------------------------

(define CTRL    #x0000)   ; device control
(define STATUS  #x0008)   ; device status (LU link-up = bit1)
(define EERD    #x0014)   ; EEPROM read (unused -- we read MAC from RAL/RAH)
(define ICR     #x00C0)   ; interrupt cause read (read-to-clear)
(define IMS     #x00D0)   ; interrupt mask set/read
(define IMC     #x00D8)   ; interrupt mask clear
(define RCTL    #x0100)   ; receive control
(define RDBAL   #x2800)   ; rx desc base low
(define RDBAH   #x2804)   ; rx desc base high
(define RDLEN   #x2808)   ; rx desc ring length (bytes)
(define RDH     #x2810)   ; rx desc head
(define RDT     #x2818)   ; rx desc tail
(define TCTL    #x0400)   ; transmit control
(define TIPG    #x0410)   ; transmit inter-packet gap
(define TDBAL   #x3800)   ; tx desc base low
(define TDBAH   #x3804)   ; tx desc base high
(define TDLEN   #x3808)   ; tx desc ring length (bytes)
(define TDH     #x3810)   ; tx desc head
(define TDT     #x3818)   ; tx desc tail
(define RAL0    #x5400)   ; receive address low 0  (MAC bytes 0..3)
(define RAH0    #x5404)   ; receive address high 0 (MAC bytes 4..5 + AV bit31)

;; CTRL bits.
(define CTRL-RST  #x04000000)   ; bit26 software reset
(define CTRL-SLU  (arithmetic-shift 1 6))   ; set link up
(define CTRL-ASDE (arithmetic-shift 1 5))   ; auto-speed-detect enable

;; STATUS bits.
(define STATUS-LU (arithmetic-shift 1 1))   ; link up

;; RCTL bits.
(define RCTL-EN    (arithmetic-shift 1 1))   ; receiver enable
(define RCTL-BAM   (arithmetic-shift 1 15))  ; broadcast accept
(define RCTL-BSEX  (arithmetic-shift 1 25))  ; buffer-size extension
(define RCTL-SECRC (arithmetic-shift 1 26))  ; strip ethernet CRC
;; BSIZE bits[17:16]: 00=2048, 01=1024, 10=512, 11=256 (x16 if BSEX). 2048 -> 00.
(define RCTL-BSIZE-2048 0)

;; TCTL bits / fields.
(define TCTL-EN  (arithmetic-shift 1 1))   ; transmit enable
(define TCTL-PSP (arithmetic-shift 1 3))   ; pad short packets
;; collision threshold (CT) bits[11:4] = 0x0F; collision distance (COLD)
;; bits[21:12] = 0x40 (full-duplex). Both are the Intel-manual recommended values.
(define TCTL-CT   (arithmetic-shift #x0F 4))
(define TCTL-COLD (arithmetic-shift #x40 12))
;; Recommended TIPG for copper: IPGT=10 (bits 9:0), IPGR1=8 (19:10), IPGR2=6 (28:20)
;; -> 0x0060200A.
(define TIPG-VALUE #x0060200A)

;; RAH AV (address valid).
(define RAH-AV (arithmetic-shift 1 31))

;; Interrupt causes / mask bits we care about: RXT0 (receiver timer, "rx done")
;; = bit7; also enable RXO (overrun, bit6) and LSC (link status change, bit2).
(define ICR-LSC  (arithmetic-shift 1 2))
(define ICR-RXO  (arithmetic-shift 1 6))
(define ICR-RXT0 (arithmetic-shift 1 7))
(define IMS-VALUE (bitwise-or ICR-RXT0 ICR-RXO ICR-LSC))

;; --- geometry ----------------------------------------------------------------
;; 32 descriptors x 2048-byte buffers per direction (~64KiB buffers + 512B of
;; descriptors per direction). Descriptor i points at slot i, so the completed
;; frame for descriptor i is at offset i*PKT-SIZE in the direction's buffer.
(define NRX 32)
(define NTX 32)
;; RX-pump poll interval. The pump sweeps the ring at least this often regardless
;; of interrupts -- e1000 variants differ (82540 is INTx-only with no MSI cap;
;; 82574 routes RX via MSI-X + IVAR, which we don't program), so a pure
;; interrupt-driven pump can sleep forever. A short poll makes RX work everywhere;
;; an MSI handle, when present, still wakes us immediately (msi-wait's timeout).
(define RX-POLL-NS 2000000)   ; 2 ms
(define PKT-SIZE 2048)
(define DESC-SIZE 16)              ; each ring descriptor is 16 bytes

;; --- legacy descriptor layout (pure helpers, hardware-free) ------------------
;; RX descriptor (16B): bufaddr u64@0, length u16@8, csum u16@10, status u8@12
;;   (DD=bit0, EOP=bit1), errors u8@13, special u16@14.
;; TX descriptor (16B): bufaddr u64@0, length u16@8, cso u8@10, cmd u8@11
;;   (EOP=bit0, IFCS=bit1, RS=bit3), status u8@12 (DD=bit0), css u8@13,
;;   special u16@14.
;; These take the ring buffer + descriptor index so a self-test can drive them
;; over a plain make-bytes buffer with mock phys addresses.

(define DD  (arithmetic-shift 1 0))   ; descriptor done (RX & TX status bit0)
(define EOP (arithmetic-shift 1 1))   ; end of packet (RX status bit1)

;; TX cmd bits.
(define TXCMD-EOP  (arithmetic-shift 1 0))
(define TXCMD-IFCS (arithmetic-shift 1 1))   ; insert FCS/CRC
(define TXCMD-RS   (arithmetic-shift 1 3))   ; report status (writes DD back)

(define (desc-off i) (* i DESC-SIZE))

;; Write a descriptor's 64-bit buffer address (lo@0, hi@4) and zero the rest of
;; the 16 bytes. Used by both ring initialisers.
(define (desc-set-addr! ring i buf-phys)
  (let ((o (desc-off i)))
    (bytes-u32-set! ring (+ o 0) (bitwise-and buf-phys #xFFFFFFFF))
    (bytes-u32-set! ring (+ o 4)
                    (bitwise-and (arithmetic-shift buf-phys -32) #xFFFFFFFF))
    (bytes-u32-set! ring (+ o 8)  0)    ; length/cso/cmd
    (bytes-u32-set! ring (+ o 12) 0)))  ; status/css/special

;; Initialise RX descriptor i: just the buffer address; status DD is left 0 so
;; the NIC owns it (it sets DD when it fills the buffer). Pure over the ring.
(define (rx-desc-init! ring i buf-phys)
  (desc-set-addr! ring i buf-phys))

;; Initialise TX descriptor i: buffer address, DD left 0 (driver-owned). The
;; length + cmd are stamped per-transmit by tx-desc-build!.
(define (tx-desc-init! ring i buf-phys)
  (desc-set-addr! ring i buf-phys))

;; RX: the status byte (offset 12). Pure -- a self-test pre-stamps it and checks.
(define (rx-desc-status ring i)
  (bytes-u8-ref ring (+ (desc-off i) 12)))

;; RX: filled? (DD set in the status byte). The NIC sets DD when the frame lands.
(define (rx-desc-done? ring i)
  (not (= 0 (bitwise-and (rx-desc-status ring i) DD))))

;; RX: end-of-packet? (EOP in the status byte). True for a single-buffer frame.
(define (rx-desc-eop? ring i)
  (not (= 0 (bitwise-and (rx-desc-status ring i) EOP))))

;; RX: the received frame length (length field u16@8). Includes the 14-byte
;; ethernet header; CRC already stripped when RCTL.SECRC is set.
(define (rx-desc-len ring i)
  (bytes-u16-ref ring (+ (desc-off i) 8)))

;; RX: re-arm descriptor i -- clear the status byte so the NIC owns it again.
;; (The buffer address was set once at init and is left in place.)
(define (rx-desc-rearm! ring i)
  (bytes-u32-set! ring (+ (desc-off i) 12) 0))   ; status/css/special = 0

;; TX: build (start) a transmit on descriptor i -- stamp length (u16@8), clear
;; cso (@10), set cmd = EOP|IFCS|RS (@11), and CLEAR the status byte (@12) so the
;; old DD doesn't read as already-done. The buffer address was set at init. Pure
;; (ring + index + len), so a self-test asserts the fields.
(define (tx-desc-build! ring i len)
  (let ((o (desc-off i)))
    (bytes-u16-set! ring (+ o 8) (bitwise-and len #xFFFF))   ; length
    (bytes-u8-set!  ring (+ o 10) 0)                          ; cso
    (bytes-u8-set!  ring (+ o 11)
                    (bitwise-or TXCMD-EOP TXCMD-IFCS TXCMD-RS)) ; cmd
    (bytes-u8-set!  ring (+ o 12) 0)                          ; status (clear DD)
    (bytes-u8-set!  ring (+ o 13) 0)                          ; css
    (bytes-u16-set! ring (+ o 14) 0)))                        ; special

;; TX: done? -- the NIC writes DD into the status byte (because we set RS) when
;; it has consumed the descriptor. Pure over the ring.
(define (tx-desc-done? ring i)
  (not (= 0 (bitwise-and (bytes-u8-ref ring (+ (desc-off i) 12)) DD))))

;; --- MAC ---------------------------------------------------------------------
;; Read the 6-byte MAC out of RAL0 (bytes 0..3) + RAH0 (bytes 4..5). Pure over a
;; bytes buffer whose RAL0/RAH0 offsets hold the two little-endian dwords -- the
;; mapped MMIO regs in the real path, a mock buffer in the self-test. Firmware
;; programs RAL0/RAH0 from the EEPROM at power-on, so this is valid after reset.
(define (read-mac regs)
  (let ((ral (bytes-u32-ref regs RAL0))
        (rah (bytes-u32-ref regs RAH0)))
    (list (bitwise-and ral #xFF)
          (bitwise-and (arithmetic-shift ral -8)  #xFF)
          (bitwise-and (arithmetic-shift ral -16) #xFF)
          (bitwise-and (arithmetic-shift ral -24) #xFF)
          (bitwise-and rah #xFF)
          (bitwise-and (arithmetic-shift rah -8)  #xFF))))

;; --- TX: round-robin over the NTX descriptors --------------------------------
;; Copy the frame into this slot's buffer, stamp the descriptor, advance TDT past
;; it (the NIC transmits everything between TDH and TDT), wait (bounded, yielding)
;; for DD, advance the free cell. On timeout DROP rather than overwrite a buffer
;; the NIC may still be reading (the bounded-wait, drop-on-timeout rtl TX rule).
(define (tx-send! regs txring txbuf free-cell frame len)
  (if (or (> len PKT-SIZE) (<= len 0))
      #f
      (let ((i (cell-ref free-cell)))
        (bytes-copy-into! txbuf (* i PKT-SIZE) frame len)
        (tx-desc-build! txring i len)
        ;; Hand descriptor i to the NIC: TDT = i+1 (mod NTX).
        (bytes-u32-set! regs TDT (modulo (+ i 1) NTX))
        (if (not (wait-until (lambda () (tx-desc-done? txring i)) 100000000)) ; 100ms
            #f
            (begin
              (cell-set! free-cell (modulo (+ i 1) NTX))
              'sent)))))

;; --- RX pump -----------------------------------------------------------------
;; Sweep every RX descriptor; for each NIC-filled one (DD set), hand the frame
;; (snapshotted out of the recycled rxbuf) to `handler`, re-arm the descriptor,
;; and advance RDT to it so the NIC may refill it. Sweeping all of them is
;; simplest and cannot miss a wrapped completion.
(define (rx-sweep! regs rxring rxbuf handler)
  (let loop ((i 0))
    (if (= i NRX)
        'swept
        (begin
          (if (rx-desc-done? rxring i)
              (begin
                (handler (* i PKT-SIZE) (rx-desc-len rxring i))
                (rx-desc-rearm! rxring i)
                ;; This descriptor is now free for the NIC: tail = i.
                (bytes-u32-set! regs RDT i)))
          (loop (+ i 1))))))

;; --- ring init ---------------------------------------------------------------

(define (rx-ring-init! rxring rxbuf)
  (let ((base (bytes-phys rxbuf)))
    (let loop ((i 0))
      (if (< i NRX)
          (begin (rx-desc-init! rxring i (+ base (* i PKT-SIZE))) (loop (+ i 1)))))))

(define (tx-ring-init! txring txbuf)
  (let ((base (bytes-phys txbuf)))
    (let loop ((i 0))
      (if (< i NTX)
          (begin (tx-desc-init! txring i (+ base (* i PKT-SIZE))) (loop (+ i 1)))))))

;; --- bring-up ----------------------------------------------------------------
;; e1000e-init takes the corenetwork service handle + the device's ECAM. It runs
;; INSIDE a spawned context (so wait-until/sleep yield). On any failure it logs
;; and returns #f. On success it spawns a TX context + an MSI RX pump, registers
;; with the network stack, enables interrupts LAST, and returns 'ok.
(define (e1000e-init net dev-ecam)
  (let ((ecam dev-ecam))
    (if (not ecam)
        (begin (display "[e1000e] no device present") (newline) #f)
        (let ((cfg (mmio-map ecam #x1000)))
          ;; Enable memory-space decode + bus-mastering BEFORE any MMIO access.
          (pci-enable-mem-bus-master! cfg)
          ;; Register window is BAR0. Firmware usually assigns it; if not (onboard
          ;; NIC behind a closed bridge), self-assign and re-read BAR0.
          (let* ((b0 (bar-base cfg 0))
                 (base (if (= b0 0)
                           (begin (pci-assign-bars ecam) (bar-base cfg 0))
                           b0)))
            (if (= base 0)
                (begin (display "[e1000e] no BAR0 register window") (newline) #f)
                (let ((regs (mmio-map base #x20000)))   ; 128KiB MMIO space
                  ;; Mask all interrupts, then software reset (bounded so a wedged
                  ;; NIC can't hang boot). The reset clears CTRL.RST itself.
                  (bytes-u32-set! regs IMC #xFFFFFFFF)
                  (bytes-u32-set! regs CTRL
                                  (bitwise-or (bytes-u32-ref regs CTRL) CTRL-RST))
                  (if (not (wait-until
                             (lambda ()
                               (= 0 (bitwise-and (bytes-u32-ref regs CTRL) CTRL-RST)))
                             1000000000))   ; up to 1s
                      (begin (display "[e1000e] reset timed out") (newline) #f)
                      (begin
                        ;; Mask interrupts again (reset may re-arm some), then
                        ;; set link up + auto-speed-detect.
                        (bytes-u32-set! regs IMC #xFFFFFFFF)
                        (bytes-u32-set! regs CTRL
                          (bitwise-or (bytes-u32-ref regs CTRL) CTRL-SLU CTRL-ASDE))
                        (let ((mac (read-mac regs)))
                          ;; Allocate rings + buffers (<4GiB phys for the 82540).
                          (let ((rxring (dma-alloc-32 (* NRX DESC-SIZE)))
                                (txring (dma-alloc-32 (* NTX DESC-SIZE)))
                                (rxbuf  (dma-alloc-32 (* NRX PKT-SIZE)))
                                (txbuf  (dma-alloc-32 (* NTX PKT-SIZE))))
                            (if (or (not rxring) (not txring) (not rxbuf) (not txbuf))
                                (begin (display "[e1000e] DMA buffer alloc failed") (newline) #f)
                                (begin
                                  (rx-ring-init! rxring rxbuf)
                                  (tx-ring-init! txring txbuf)
                                  ;; Program the RX ring registers. RDH=0, RDT=last
                                  ;; descriptor (NRX-1): the NIC fills [RDH..RDT).
                                  (bytes-u32-set! regs RDBAL (bitwise-and (bytes-phys rxring) #xFFFFFFFF))
                                  (bytes-u32-set! regs RDBAH (bitwise-and (arithmetic-shift (bytes-phys rxring) -32) #xFFFFFFFF))
                                  (bytes-u32-set! regs RDLEN (* NRX DESC-SIZE))
                                  (bytes-u32-set! regs RDH 0)
                                  (bytes-u32-set! regs RDT (- NRX 1))
                                  (bytes-u32-set! regs RCTL
                                    (bitwise-or RCTL-EN RCTL-BAM RCTL-SECRC RCTL-BSIZE-2048))
                                  ;; Program the TX ring registers. TDH=TDT=0 (empty).
                                  (bytes-u32-set! regs TDBAL (bitwise-and (bytes-phys txring) #xFFFFFFFF))
                                  (bytes-u32-set! regs TDBAH (bitwise-and (arithmetic-shift (bytes-phys txring) -32) #xFFFFFFFF))
                                  (bytes-u32-set! regs TDLEN (* NTX DESC-SIZE))
                                  (bytes-u32-set! regs TDH 0)
                                  (bytes-u32-set! regs TDT 0)
                                  (bytes-u32-set! regs TIPG TIPG-VALUE)
                                  (bytes-u32-set! regs TCTL
                                    (bitwise-or TCTL-EN TCTL-PSP TCTL-CT TCTL-COLD))
                                  ;; MSI is OPTIONAL: pci-setup-msi returns #f on a
                                  ;; device with no MSI cap (QEMU e1000/82540 is
                                  ;; INTx-only). We poll either way, so a missing
                                  ;; MSI is not fatal -- it just removes the early
                                  ;; wake.
                                  (let ((msi (pci-setup-msi ecam)))
                                    (let ((free-cell (make-cell 0)))
                                      ;; TX context: (tx frame len reply?) -> copy + post.
                                      (let ((tx-ctx
                                              (spawn-restricted '() (lambda ()
                                                (let loop ()
                                                  (let ((m (recv)))
                                                    (if (eq? (car m) 'tx)
                                                        (begin
                                                          (tx-send! regs txring txbuf free-cell (cadr m) (caddr m))
                                                          (if (> (length m) 3) (send (nth m 3) (list 'tx-done)))))
                                                    (loop)))))))
                                        ;; RX pump: POLL the ring (sweep, then wait).
                                        ;; With an MSI handle we park on it with a poll
                                        ;; timeout so a real interrupt wakes us at once
                                        ;; but an unrouted one is still swept within
                                        ;; RX-POLL-NS; with no MSI we sleep-poll. The pump
                                        ;; is the sole writer of ring/RDT state and holds
                                        ;; NOTHING across (send net ...) (corenetwork
                                        ;; re-enters our TX path for ARP/ICMP replies).
                                        (spawn-restricted '() (lambda ()
                                          (let loop ()
                                            (bytes-u32-ref regs ICR)   ; ack (read-to-clear)
                                            (rx-sweep! regs rxring rxbuf
                                              (lambda (off len)
                                                (send net (list 'rx (copy-bytes rxbuf off len) len))))
                                            (if msi (msi-wait msi (msi-count msi) RX-POLL-NS) (sleep RX-POLL-NS))
                                            (loop))))
                                        ;; Announce to the stack: MAC + the TX context.
                                        (send net (list 'register-nic mac tx-ctx))
                                        ;; Enable RX interrupt generation only when MSI is
                                        ;; wired (an unhandled INTx on the no-MSI path could
                                        ;; storm); the poll covers delivery either way.
                                        (bytes-u32-ref regs ICR)            ; clear stale causes
                                        (if msi (bytes-u32-set! regs IMS IMS-VALUE)) ; RXT0|RXO|LSC
                                        (display "[e1000e] registered with network stack, mac=")
                                        (display mac) (newline)
                                        'ok))))))))))))))))) ; closes (define-module e1000e ...)
