# Debugging Cardinal; with GDB over serial

`SysGdb` is a GDB Remote Serial Protocol stub. It lets you attach GDB to the
running OS over a serial channel and inspect/modify registers and memory, set
breakpoints, single-step, and continue. Two channels are supported: the built-in
**COM2** UART, and a **USB-serial (FTDI)** adapter — the latter works on real
hardware with no native serial port.

## What works

- Read/write registers (`g`/`G`, `info registers`, `$rax = ...`).
- Read/write memory (`m`/`M`, `x`, `set {int}addr = ...`), disassembly (`x/i`).
- Software breakpoints + single-step + continue (GDB drives these via memory
  writes and the TF flag; no `Z` packets needed).

Validated under QEMU/KVM over both COM2 and a USB-serial adapter: GDB read sane
registers, disassembled `$rip`, and resumed the OS.

## Over COM2 (built-in serial)

Debug output stays on COM1; GDB uses COM2. Map COM2 to a socket and connect GDB:

```bash
qemu-system-x86_64 ... \
  -serial file:debug.log \                              # COM1: kernel debug output
  -serial tcp:127.0.0.1:1234,server,nowait              # COM2: GDB
gdb build/kernel/kernel.bin -ex 'target remote :1234'
```

To stop at boot and wait for GDB, add `CALL:gdb_stub_wait` to `loadscript.txt`
(it raises a breakpoint). Otherwise, place `int3` / call `gdb_stub_wait()` in the
code you want to stop at, or `break` from GDB once attached.

## Over a USB-serial adapter (FTDI; works on real hardware)

`drivers/usb_serial` brings up an FTDI USB-serial adapter via the USB stack,
routes the GDB stub over it, and runs a small monitor: while no debugger is
connected the system runs normally; the moment GDB sends traffic over the
adapter the monitor drops into the stub, and GDB drives the session from there.

QEMU (the adapter's wire is a socket GDB connects to):
```bash
qemu-system-x86_64 ... \
  -device qemu-xhci,id=xhci \
  -chardev socket,id=g,host=127.0.0.1,port=1239,server=on,wait=off \
  -device usb-serial,bus=xhci.0,chardev=g
gdb build/kernel/kernel.bin -ex 'target remote :1239'
```

On real hardware: plug an FTDI USB-serial dongle into the target, connect its
far end to the host running GDB (e.g. a second USB-serial + null modem), and
`target remote <port>`.

## Architecture

- `SysGdb` (`modules/SysGdb`) registers handlers for #BP (vector 3) and #DB
  (vector 1); on a trap it talks RSP over a pluggable channel (default COM2). It
  reads/writes the trapped frame via SysInterrupts `get/setregisterstate`, so
  steps and register writes take effect on `iret`.
- `gdb_set_channel(getc, putc)` swaps the channel; `drivers/usb_serial` calls it
  to route over FTDI.

## Limitations / future work

- Single-core: entering the stub freezes the core (interrupts off). Other cores
  keep running; a full-machine stop would need a halt IPI.
- USB-serial re-attach after `detach` needs a reboot (the monitor breaks in once
  to avoid a poll-vs-breakpoint lock race). An async Ctrl-C break-in is future
  work.
- `m`/`M` read/write memory directly; a fault on a bad address is not yet
  recovered (GDB normally only touches valid addresses).
- Only software breakpoints (no hardware watchpoints).
