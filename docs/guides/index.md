# How-to Guides

Task-oriented guides for getting a specific job done. Unlike the
[tutorials](../tutorials/index.md), these assume you already have a working build and
some familiarity with the system.

- [Add a PCI driver](add-a-pci-driver.md) — BAR mapping, MSI setup, DMA, RX/TX, and
  registering with the right `Core*` service (plus the RX-handler locking rule).
- [Add a Core\* server & design a message protocol](add-a-server.md) — port binding,
  request/reply, capability-hardened handles, and the common messaging idioms.
- [Debug the OS](debugging.md) — the serial REPL, the `sys-debug` live context
  inspector, and the GDB remote stub.

More guides (storage, graphics, audio, networking, USB, module signing) are planned;
see the [documentation roadmap](../index.md).
