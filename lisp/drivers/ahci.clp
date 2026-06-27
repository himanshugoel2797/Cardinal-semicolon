;; ahci: a SATA AHCI block-device driver in Cardinal Lisp, ported from drivers/ahci.
;;
;; The C driver does only HBA reset + port init + READ DMA EXT (no IDENTIFY, no
;; WRITE). This port adds IDENTIFY (0xEC) and WRITE DMA EXT (0x35) from the ATA
;; spec, following the on-wire H2D register-FIS byte layout (authoritative), and
;; wires the result into the corestorage block-device registry: it answers
;; (read lba count reply) / (write lba count data reply) the same way the C
;; storage stack drove read/write function pointers.
;;
;; Structure (the include parts under lisp/drivers/ahci/):
;;   hba    -- HBA global registers: CAP/S64A decode, BIOS/OS handoff (gated on
;;             CAP2.BOH), HBA reset (GHC.HR), AHCI enable (GHC.AE), PI port scan.
;;   port   -- the per-port DMA region layout (command list / received FIS /
;;             command table + PRDT), the command-header builder, and port
;;             idle/link/ready bring-up (FRE then ST).
;;   ata    -- the H2D register FIS builder, the PRDT setter, issue! (the
;;             build-FIS + clear-PxIS + set-PxCI + wait-PxCI-clear dance, PxCI
;;             clearing being the authoritative completion signal), IDENTIFY /
;;             READ / WRITE, and the IDENTIFY parser (LBA48 sector count, model).
;;   driver -- discovery, the spawned (yielding) bring-up context, the block-device
;;             driver context, and corestorage registration.
;;
;; CAVEAT -- class binding. pci-find matches VENDOR/DEVICE id only, so this binds
;; QEMU's ICH9 AHCI controller (8086:2922) specifically. A class-based pci-find
;; (match any 01/06 AHCI HBA) is a noted future substrate addition -- it is NOT
;; built here, by design.
;;
;; The driver imports exactly the capabilities it needs -- sys-mmio (mmio-map /
;; dma-alloc / dma-alloc-32), sys-pci (pci-find / pci-assign-bars / pci-setup-msi
;; + the MSI wake bridge msi-count / msi-wait) -- plus the generic driver-util
;; helpers. ALL reset / spin-up / completion waits YIELD (wait-until / sleep /
;; msi-wait with a timeout); the bring-up runs in a spawned context so the yields
;; have somewhere to go. It exports the entry point ahci-init plus the pure FIS /
;; PRDT builders and the IDENTIFY parser for the hardware-free self-test.
(define-module ahci
  (export ahci-init
          fis-build! prdt-set! cmdhdr-set!
          id-sector-count id-model id-word
          ATA-IDENTIFY ATA-READ-EXT ATA-WRITE-EXT
          PRDT-OFF FIS-OFF)
  (import sys-mmio sys-pci driver-util)
  (define lg (make-logger 'ahci))
  (include hba port ata driver))
