# Drivers

Drivers live under `lisp/drivers/` and are bound to hardware by `lisp/init.clp`
(each gated on a `pci-find`). A driver's `module_init`-equivalent entry receives its
device, sets up DMA/MSI, and registers with the relevant `Core*` server (a NIC calls
`network-register`, a display driver calls `display-register`, etc.).

| Driver | Device class |
|--------|--------------|
| [ahci](ahci.md) | SATA AHCI block storage |
| [virtio-net](virtio-net.md) | virtio network |
| [virtio-gpu](virtio-gpu.md) | virtio GPU / display |
| [rtl8139](rtl8139.md) | Realtek 8139 NIC |
| [rtl8169](rtl8169.md) | Realtek 8169/8111 NIC |
| [lfb](lfb.md) | Linear framebuffer display |
| [hdaudio](hdaudio.md) | Intel HD Audio |
| [ps2](ps2.md) | PS/2 keyboard/mouse |
| [uhci](uhci.md) | UHCI USB controller |
| [ehci](ehci.md) | EHCI USB controller |
| [xhci](xhci.md) | xHCI USB controller |
| [usb-hid](usb-hid.md) | USB HID class |
| [usb-hub](usb-hub.md) | USB hub class |
| [usb-storage](usb-storage.md) | USB mass-storage class |
| [usb-audio](usb-audio.md) | USB audio (UAC1) class |
