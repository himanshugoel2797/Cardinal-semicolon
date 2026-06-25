;; virtio-net: the first device driver written in Cardinal Lisp.
;;
;; Imported at boot by the `init` module (single-core); init calls (virtio-net-init)
;; on the BSP once the scheduler is live. The device-agnostic virtio 1.0 transport
;; (the PCI capability walk, the status/feature handshake, split virtqueue setup,
;; the notify kick) now lives in the shared `virtio` library; this module is the
;; net-SPECIFIC half: the F_MAC feature, the virtio-net header, the RX/TX ring
;; population, and the contexts that pump frames to/from the network service.
;;
;;   N2: discover, negotiate features, set up the RX/TX virtqueues, DRIVER_OK, MAC.
;;   N3: MSI-X -> ISR -> a Lisp RX context draining the used ring.
;;   N4: TX an ARP request and recognise the reply -> both directions end-to-end.

;; The driver imports exactly the capabilities it needs -- sys-mmio (mmio-map/
;; dma-alloc) and sys-pci (pci-find/pci-setup-msi + the MSI wake bridge msi-count/
;; msi-wait) -- plus the generic driver-util helpers and the shared virtio
;; transport. It exports just the entry point virtio-net-init.
(define-module virtio-net
  (export virtio-net-init)
  (import sys-mmio sys-pci driver-util virtio)

;; --- RX -------------------------------------------------------------------
;; One contiguous RX buffer holds NRX slots of RXSLOT bytes; descriptor i points
;; at slot i, so the completed frame for descriptor id is at offset id*RXSLOT.

(define NRX 16)
(define RXSLOT 2048)         ; 12-byte virtio-net header + up to a 1514 frame
(define VNET-HDR 12)         ; virtio_net_hdr is 12 bytes in virtio 1.0 (num_buffers present)

;; Fill the RX descriptors (device-writable) and make them all available.
(define (rx-populate! rxq rxbuf notify mult)
  (let ((base (bytes-phys rxbuf)) (desc (q-desc rxq)) (avail (q-avail rxq))
        (n (if (< (q-size rxq) NRX) (q-size rxq) NRX)))   ; never exceed the ring
    (let loop ((i 0))
      (if (= i n)
          (begin (notify-queue! notify mult rxq) 'done)
          (begin
            (desc-set! desc i (+ base (* i RXSLOT)) RXSLOT VIRTQ-DESC-F-WRITE 0)
            (avail-push! avail (q-size rxq) i)
            (loop (+ i 1)))))))

;; Drain newly-used RX descriptors, calling (handler slot-offset frame-len) for
;; each received frame, then recycle the descriptor back into the avail ring.
(define (rx-drain! rxq rxbuf last notify mult handler)
  (let ((used (q-used rxq)) (avail (q-avail rxq)) (qsize (q-size rxq)))
    (let loop ((li (cell-ref last)))
      (if (= li (bytes-u16-ref used 2))
          (cell-set! last li)
          (let* ((slot (modulo li qsize))
                 (id   (bytes-u32-ref used (+ 4 (* 8 slot))))
                 (ulen (bytes-u32-ref used (+ 8 (* 8 slot)))))
            (handler (+ (* id RXSLOT) VNET-HDR) (- ulen VNET-HDR))  ; FRAME offset
            (avail-push! avail qsize id)          ; recycle the buffer
            (notify-queue! notify mult rxq)
            (loop (bitwise-and (+ li 1) #xFFFF)))))))

;; --- TX -------------------------------------------------------------------
;; A single TX buffer: [VNET-HDR zero bytes][ethernet frame]. Post on descriptor 0.

(define (tx-frame! txq txbuf notify mult frame-len)
  (let loop ((k 0)) (if (< k VNET-HDR) (begin (bytes-u8-set! txbuf k 0) (loop (+ k 1))) 'z))
  (desc-set! (q-desc txq) 0 (bytes-phys txbuf) (+ VNET-HDR frame-len) 0 0)
  (avail-push! (q-avail txq) (q-size txq) 0)
  (notify-queue! notify mult txq))

;; All ethernet/ARP/IP framing now lives in the corenetwork service; this driver
;; is pure transport. put-be16! and the byte-copy helpers come from driver-util.

;; --- bring-up ---------------------------------------------------------------

(define VIRTIO-NET-VID #x1af4)
(define VIRTIO-NET-DID #x1041)
(define VIRTIO-NET-F-MAC-BIT 5)

(define (virtio-net-read-mac devcfg)
  (list (bytes-u8-ref devcfg 0) (bytes-u8-ref devcfg 1) (bytes-u8-ref devcfg 2)
        (bytes-u8-ref devcfg 3) (bytes-u8-ref devcfg 4) (bytes-u8-ref devcfg 5)))

;; virtio-net-init takes the corenetwork service handle: the NIC is now pure
;; transport, so it registers itself with the network stack (handing it the MAC
;; and a TX context) and forwards every received frame to it, rather than doing
;; any ethernet/ARP decoding itself.
;; `dev-ecam` is the device's ECAM, supplied by init (which enumerates NICs via
;; pci-find-all and binds the driver to each), so one driver can drive several NICs.
(define (virtio-net-init net dev-ecam)
  (let ((ecam dev-ecam))
    (if (not ecam)
        (begin (display "[virtio-net] no device present") (newline) #f)
        ;; Common transport bring-up: accept F_MAC (lo) + VERSION_1 (hi).
        (let ((dev (virtio-bringup ecam
                     (arithmetic-shift 1 VIRTIO-NET-F-MAC-BIT)
                     (arithmetic-shift 1 VIRTIO-F-VERSION-1-BIT))))
          (if (not dev)
              (begin (display "[virtio-net] device rejected FEATURES_OK") (newline) #f)
              (let ((common (nth dev 0)) (devcfg (nth dev 1))
                    (notify (nth dev 2)) (mult (nth dev 3)))
                ;; Set up the queues first -- virtio-setup-queue programs each
                ;; queue_msix_vector -- then msix_config, and only THEN enable the
                ;; MSI-X capability (pci-setup-msi): the spec wants virtio's vector
                ;; registers configured before the cap is enabled.
                (let ((rxq (virtio-setup-queue common 0))
                      (txq (virtio-setup-queue common 1))
                      (mac (virtio-net-read-mac devcfg)))
                  (bytes-u16-set! common VIRTIO-MSIX-CONFIG 0)    ; config events -> entry 0
                  (let ((msi (pci-setup-msi ecam)))    ; -> a per-device MSI handle
                    (if (not msi)
                        (begin (display "[virtio-net] MSI-X setup failed") (newline) #f)
                        (begin
                          (virtio-status-set! common VIRTIO-STATUS-DRIVER-OK)
                          (display "[virtio-net] up: mac=") (display mac)
                          (display " msi=") (display msi) (newline)
                          (let ((rxbuf (dma-alloc (* NRX RXSLOT)))
                                (txbuf (dma-alloc RXSLOT))
                                (last  (make-cell 0)))
                            (rx-populate! rxq rxbuf notify mult)
                            ;; TX context: the network service sends (tx frame len);
                            ;; copy the ethernet frame into txbuf after the VNET
                            ;; header and post it. (Single TX buffer -> one frame in
                            ;; flight; fine for the current low-rate control traffic,
                            ;; a TX ring is a later refinement.)
                            (let ((tx-ctx
                                    (spawn-restricted '() (lambda ()
                                      (let loop ()
                                        (let ((m (recv)))
                                          (if (eq? (car m) 'tx)
                                              (begin
                                                (bytes-copy-into! txbuf VNET-HDR (cadr m) (caddr m))
                                                (tx-frame! txq txbuf notify mult (caddr m))))
                                          (loop)))))))
                              ;; RX context: drain received frames and forward each
                              ;; (snapshotted out of the recycled rxbuf) to the
                              ;; network service. Both contexts get the empty grant.
                              (spawn-restricted '() (lambda ()
                                (let loop ((seen (msi-count msi)))
                                  (rx-drain! rxq rxbuf last notify mult
                                    (lambda (off len)
                                      (send net (list 'rx (copy-bytes rxbuf off len) len))))
                                  (if (> (msi-count msi) seen)
                                      (loop (msi-count msi))
                                      (begin (msi-wait msi seen) (loop (msi-count msi)))))))
                              ;; Announce ourselves to the stack: MAC + the TX context.
                              (send net (list 'register-nic mac tx-ctx))
                              (display "[virtio-net] registered with network stack") (newline)
                              'ok))))))))))))) ; last ) closes (define-module virtio-net ...)
