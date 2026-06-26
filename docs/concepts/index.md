# Concepts & Architecture

Understanding-oriented articles: the mental models behind **Cardinal;**. Read these
when you want to know *why* the system is shaped the way it is, not just how to call
an API.

- [System overview](system-overview.md) — the microkernel, signed loadable modules,
  and the Lisp OS layer, and how the pieces relate.
- [Capabilities & the sandbox](capabilities-and-sandbox.md) — how a Lisp context is
  the unit of isolation, why that *is* userspace (and is not ring-3), and how
  `import` and grants gate authority.
- [Message passing & concurrency](message-passing.md) — contexts, the scheduler,
  `send`/`recv`/ports, and the gotchas of building servers on them.

These synthesize the internal design notes under `notes/`; the
[reference](../servers/index.md) documents the concrete APIs they describe.
