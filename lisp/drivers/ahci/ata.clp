;; ahci/ata.clp -- the ATA command layer: the H2D register FIS, the PRDT, the
;; issue/poll dance, and the IDENTIFY/READ/WRITE commands + the IDENTIFY parser.
;;
;; The on-wire Host-to-Device register FIS (ATA8-AAM / SATA) is 20 bytes:
;;   byte 0  : FIS type = 0x27 (Register H2D)
;;   byte 1  : bit7 = C (1 = command FIS, not a control update); bits0-3 PM port = 0
;;   byte 2  : command (0xEC IDENTIFY, 0x25 READ DMA EXT, 0x35 WRITE DMA EXT)
;;   byte 3  : features (7:0)
;;   byte 4  : LBA  7:0          \
;;   byte 5  : LBA 15:8           > the low 24 LBA bits
;;   byte 6  : LBA 23:16         /
;;   byte 7  : device (0x40 = LBA mode, the standard LBA48 device byte)
;;   byte 8  : LBA 31:24         \
;;   byte 9  : LBA 39:32          > the high 24 LBA bits (LBA48)
;;   byte 10 : LBA 47:40         /
;;   byte 11 : features 15:8
;;   byte 12 : count  7:0
;;   byte 13 : count 15:8
;;   byte 14 : ICC
;;   byte 15 : control
;;   bytes 16-19: reserved (0)
;; This byte layout is authoritative; we lay it out a byte at a time so the result
;; is endianness-independent (these are not multi-byte little-endian fields, they
;; are positional bytes on the wire).

(define ATA-IDENTIFY  #xEC)
(define ATA-READ-EXT  #x25)
(define ATA-WRITE-EXT #x35)

;; Build an H2D register FIS into `fis` at offset 0. lba is a 48-bit integer, count
;; is the sector count (0 means 65536 for the DMA-EXT commands; IDENTIFY uses 0).
;; device defaults to 0x40 (LBA mode) for the data commands; IDENTIFY passes 0.
(define (fis-build! fis command lba count device)
  ;; Zero the 20-byte FIS first (the table buffer may be reused).
  (let loop ((i 0)) (if (< i 20) (begin (bytes-u8-set! fis i 0) (loop (+ i 1))) 'z))
  (bytes-u8-set! fis 0  #x27)                              ; FIS type = Register H2D
  (bytes-u8-set! fis 1  #x80)                              ; C bit set (command)
  (bytes-u8-set! fis 2  command)
  (bytes-u8-set! fis 4  (bitwise-and lba #xFF))            ; LBA 7:0
  (bytes-u8-set! fis 5  (bitwise-and (arithmetic-shift lba -8)  #xFF))   ; 15:8
  (bytes-u8-set! fis 6  (bitwise-and (arithmetic-shift lba -16) #xFF))   ; 23:16
  (bytes-u8-set! fis 7  device)
  (bytes-u8-set! fis 8  (bitwise-and (arithmetic-shift lba -24) #xFF))   ; 31:24
  (bytes-u8-set! fis 9  (bitwise-and (arithmetic-shift lba -32) #xFF))   ; 39:32
  (bytes-u8-set! fis 10 (bitwise-and (arithmetic-shift lba -40) #xFF))   ; 47:40
  (bytes-u8-set! fis 12 (bitwise-and count #xFF))         ; count 7:0
  (bytes-u8-set! fis 13 (bitwise-and (arithmetic-shift count -8) #xFF))  ; 15:8
  fis)

;; Set PRDT entry 0 in the command table (entry base = table offset 0x80):
;;   dword0 = DBA (data base, lo); dword1 = DBAU (data base, hi);
;;   dword2 = reserved; dword3 = DBC (byte count - 1) with bit31 = interrupt.
;; len is the data byte count (must be even, max 4MB per entry); we always set the
;; interrupt-on-completion bit so the HBA raises an interrupt when this PRD drains.
(define (prdt-set! ctbl data-phys len s64a?)
  (bytes-u32-set! ctbl (+ PRDT-OFF 0)  (bitwise-and data-phys #xFFFFFFFF))
  (bytes-u32-set! ctbl (+ PRDT-OFF 4)  (if s64a? (arithmetic-shift data-phys -32) 0))
  (bytes-u32-set! ctbl (+ PRDT-OFF 8)  0)
  (bytes-u32-set! ctbl (+ PRDT-OFF 12) (bitwise-or (bitwise-and (- len 1) #x3FFFFF)
                                                   #x80000000))   ; bit31 = I
  ctbl)

;; --- issue + wait ------------------------------------------------------------
;; Build the command into the table + header, clear PxIS, snapshot the MSI count,
;; raise PxCI bit0, then wait for PxCI bit0 to self-clear (the AUTHORITATIVE
;; completion signal) -- yielding via msi-wait between checks when an MSI handle is
;; given, else via wait-until's sleep. msi just trims latency; the poll is truth.
;; Returns 0 on success, -1 on a device error (PxTFD.ERR) or a completion timeout.
;;
;; ctx = (abar port regions s64a? msi) -- msi may be #f (poll-only, used in P1).
(define (ahci-ctx-abar c)    (nth c 0))
(define (ahci-ctx-port c)    (nth c 1))
(define (ahci-ctx-regions c) (nth c 2))
(define (ahci-ctx-s64a c)    (nth c 3))
(define (ahci-ctx-msi c)     (nth c 4))

(define (ci-clear? abar port) (= 0 (bitwise-and (px-rd abar port PxCI) 1)))

(define (issue! ctx command lba count device write? data-phys data-len)
  (let ((abar    (ahci-ctx-abar ctx))
        (port    (ahci-ctx-port ctx))
        (regions (ahci-ctx-regions ctx))
        (s64a?   (ahci-ctx-s64a ctx))
        (msi     (ahci-ctx-msi ctx)))
    (let ((cmdlist (regs-cmdlist regions))
          (cmdtbl  (regs-cmdtbl regions))
          (ctbl-phys (bytes-phys (regs-cmdtbl regions))))
      ;; Build the FIS + PRDT, then the slot-0 command header (5-dword FIS, 1 PRD).
      (fis-build! cmdtbl command lba count device)
      (prdt-set! cmdtbl data-phys data-len s64a?)
      (cmdhdr-set! cmdlist ctbl-phys 5 write? 1)
      ;; Clear interrupt status, snapshot MSI, raise the command.
      (px-wr abar port PxIS #xFFFFFFFF)
      (let ((seen (if msi (msi-count msi) 0)))
        (px-wr abar port PxCI 1)
        ;; Park until PxCI bit0 clears, bounded by a 3s deadline. The inner
        ;; msi-wait/sleep yields the core; the OUTER wait-until poll is the real
        ;; completion test, so a missed/coalesced MSI never wedges us.
        (let ((done
               (let ((deadline (+ (uptime-ns) 3000000000)))
                 (let loop ()
                   (cond ((ci-clear? abar port) #t)
                         ((> (uptime-ns) deadline) #f)
                         (else
                          (if msi
                              (msi-wait msi seen 50000000)   ; wake on MSI or 50ms
                              (sleep 200000))
                          (loop)))))))
          (cond ((not done) -1)              ; completion timeout
                ((not (= 0 (bitwise-and (px-rd abar port PxTFD) PxTFD-ERR))) -1)
                (else 0)))))))

;; --- commands ---------------------------------------------------------------
;; IDENTIFY (0xEC): 512 bytes -> the data buffer; device byte 0, count 0.
(define (identify ctx data-phys)
  (issue! ctx ATA-IDENTIFY 0 0 0 #f data-phys 512))

;; READ DMA EXT (0x25): `count` sectors from `lba` -> data buffer (count*512 bytes).
;; device 0x40 = LBA mode (the LBA48 device byte).
(define (read-sectors ctx lba count data-phys)
  (issue! ctx ATA-READ-EXT lba count #x40 #f data-phys (* count 512)))

;; WRITE DMA EXT (0x35): `count` sectors from the data buffer -> `lba`. write? = #t
;; so the command header marks the transfer host->device.
(define (write-sectors ctx lba count data-phys)
  (issue! ctx ATA-WRITE-EXT lba count #x40 #t data-phys (* count 512)))

;; --- IDENTIFY parse ---------------------------------------------------------
;; The IDENTIFY data is 256 little-endian 16-bit words. Word w is at byte 2*w.
(define (id-word buf w) (bytes-u16-ref buf (* w 2)))

;; Total addressable sectors: if word 83 bit10 (LBA48 supported) is set, the count
;; is the 48-bit value in words 100-103; otherwise the LBA28 count in words 60-61.
(define (id-sector-count buf)
  (if (not (= 0 (bitwise-and (id-word buf 83) #x400)))
      (bitwise-or (id-word buf 100)
                  (arithmetic-shift (id-word buf 101) 16)
                  (arithmetic-shift (id-word buf 102) 32)
                  (arithmetic-shift (id-word buf 103) 48))
      (bitwise-or (id-word buf 60)
                  (arithmetic-shift (id-word buf 61) 16))))

;; The model string is words 27..46 (40 ASCII bytes), but each word is stored
;; byte-swapped (high byte first), so swap each word back to recover the text.
;; Returns the model as a string with trailing spaces trimmed.
(define (id-model buf)
  (let ((chars
         (let loop ((w 27) (acc '()))
           (if (> w 46)
               (reverse acc)
               (let ((word (id-word buf w)))
                 (loop (+ w 1)
                       (cons (bitwise-and word #xFF)               ; low byte (2nd)
                             (cons (arithmetic-shift word -8) acc)))))))) ; high (1st)
    ;; Trim trailing spaces (0x20) and NULs, then build the string.
    (list->string (map (lambda (c) (integer->char c)) (trim-trailing chars)))))

;; Drop trailing 0x20/0x00 bytes from a list of byte values.
(define (trim-trailing lst)
  (reverse (let drop ((r (reverse lst)))
             (cond ((null? r) r)
                   ((or (= (car r) #x20) (= (car r) 0)) (drop (cdr r)))
                   (else r)))))
