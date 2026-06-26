# Servers

The `Core*` services are Lisp contexts under `lisp/servers/`. Each runs in its own
capability-gated VM context, owns some piece of OS state, and exposes its API as a
**message protocol** (drivers and other servers `send` it requests and `recv`
replies) rather than a C ABI.

| Server | Role |
|--------|------|
| [coreinput](coreinput.md) | Input event hub (keyboard/pointer) |
| [coreaudio](coreaudio.md) | Audio mixing / playback + capture |
| [corepower](corepower.md) | Power management |
| [corestorage](corestorage.md) | Block-device registry |
| [cardfs](cardfs.md) | `cardfs` object-store filesystem |
| [coredisplay](coredisplay.md) | Display/framebuffer service |
| [corecompositor](corecompositor.md) | Multi-client window compositor |
| [corenetwork](corenetwork.md) | ARP/ICMP/IPv4 + UDP + RDT + TCP/DHCP/DNS |
| [corenetdebug](corenetdebug.md) | Network debug transport |
| [coreusb](coreusb.md) | USB enumeration + class-driver dispatch |
