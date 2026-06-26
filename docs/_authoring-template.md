<!--
AUTHORING TEMPLATE — not published as a real page; remove or keep as reference.

Every server/driver API-reference page should follow this shape so the site reads
uniformly. Copy the headings below and fill them from the actual source. Omit a
section only if it genuinely does not apply (say so in one line rather than leaving
an empty heading).

Conventions:
- Document what the source ACTUALLY does — read the .clp (and any include files in a
  same-named subdirectory). Do not invent messages or functions. If something is a
  stub/TODO, say so.
- Code fences use ```scheme.
- Refer to other pages with relative links, e.g. [corenetwork](corenetwork.md).
- "Capabilities" = which `import`able sys-* prim modules / grants the context needs
  (the VM gates `import` per context). "Messages" = the request/reply protocol other
  contexts use via `send`/`recv`.
-->

# <component-name>

> One-sentence summary of what this server/driver is and does.

| | |
|---|---|
| **Source** | `lisp/servers/<name>.clp` (+ `lisp/servers/<name>/*.clp` if split) |
| **Kind** | server &#124; driver |
| **Bound by** | `lisp/init.clp` — gated on `(pci-find …)` / always / on demand |
| **Registers with** | e.g. CoreDisplay via `display-register`, or "n/a" |
| **Capabilities** | sys-* modules / grants this context imports |

## Overview

A paragraph or two: what state it owns, where it sits in the stack, who talks to it.

## Initialization

The entry point(s) `init.clp` calls, with signatures and what each argument is.

```scheme
(<name>-init <args…>)   ; → what it returns / what it spawns
```

## Message protocol

For each request the context handles, document the tag, the payload fields, and the
reply. Use a subsection per message.

### `:<message-tag>`

- **Request:** `(:tag field-a field-b)` — meaning of each field.
- **Reply:** what is sent back (or "no reply" / "ack").
- **Errors:** failure modes.

## Exported functions

Public `define`d functions other modules `import` and call directly (as opposed to
message-driven). Signature + one-line description each.

### `(<fn> <args>)`

What it does, arguments, return value, side effects.

## Notes / gotchas

Anything load-bearing: locking rules, ordering constraints, known stubs, hardware
quirks. Link to `notes/AUDIT.md` items where relevant.
