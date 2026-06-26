# Tutorials

Learning-oriented, end-to-end walkthroughs. Start here if you're new to
**Cardinal;** — each tutorial takes you from nothing to a working result, in order.

1. [Build & boot Cardinal; in QEMU](build-and-boot.md) — get the toolchain up, build
   the kernel + modules, and reach the serial REPL.
2. [Your first REPL session](first-repl-session.md) — drive the live system from the
   COM1 Lisp REPL: evaluate code, inspect live contexts with `sys-debug`, and send a
   message to a running server.
3. [Write your first driver](first-driver.md) — bind a PCI device in `init.clp` and
   register it with a `Core*` service, end to end.

Once you've worked through these, the [how-to guides](../guides/index.md) cover
specific tasks in depth, and the [concept articles](../concepts/index.md) explain how
the system fits together.
