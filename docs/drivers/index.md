# Drivers

Drivers live under `lisp/drivers/` and are bound to hardware by `lisp/init.clp`
(each gated on a `pci-find`). A driver's init entry receives its device, sets up
DMA/MSI, and registers with the relevant `Core*` server by message passing (a NIC
registers with [corenetwork](../servers/corenetwork.md), a display driver with
[coredisplay](../servers/coredisplay.md), etc.).

| Driver | Device class |
|--------|--------------|
| [ahci](ahci.md) | SATA AHCI block storage |
| [nvme](nvme.md) | NVMe PCIe block storage |
| [virtio-blk](virtio-blk.md) | virtio block storage |
| [virtio-net](virtio-net.md) | virtio network |
| [e1000e](e1000e.md) | Intel e1000 / e1000e NIC |
| [virtio-gpu](virtio-gpu.md) | virtio GPU / display |
| [rtl8139](rtl8139.md) | Realtek 8139 NIC |
| [rtl8169](rtl8169.md) | Realtek 8169/8111 NIC |
| [lfb](lfb.md) | Linear framebuffer display |
| [hdaudio](hdaudio.md) | Intel HD Audio |
| [virtio-input](virtio-input.md) | virtio tablet / keyboard / mouse |
| [ps2](ps2.md) | PS/2 keyboard/mouse |
| [virtio-rng](virtio-rng.md) | virtio entropy source |
| [virtio-console](virtio-console.md) | virtio serial console |
| [uhci](uhci.md) | UHCI USB controller |
| [ehci](ehci.md) | EHCI USB controller |
| [xhci](xhci.md) | xHCI USB controller |
| [usb-hid](usb-hid.md) | USB HID class |
| [usb-hub](usb-hub.md) | USB hub class |
| [usb-storage](usb-storage.md) | USB mass-storage class |
| [usb-audio](usb-audio.md) | USB audio (UAC1) class |
