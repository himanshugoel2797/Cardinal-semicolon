// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_SYSGDB_H
#define CARDINALSEMI_SYSGDB_H

// Drop into the GDB stub and wait for a debugger (raises a breakpoint). Useful
// from a boot script to debug early boot. Returns 0 (so it works as a `CALL:`
// boot-script target).
int gdb_stub_wait(void);

// A pluggable byte transport for the GDB channel. The stub defaults to COM2; a
// driver (e.g. USB-serial) can supply its own without knowing anything about the
// GDB protocol. `getc`/`putc` are blocking byte I/O. `poll` is optional: a
// non-blocking check used for async Ctrl-C break-in on transports that have no
// receive interrupt -- it should pump the hardware and return >0 if one or more
// bytes arrived, or 0 otherwise. Leave `poll` NULL when the transport delivers
// break-in by its own means (e.g. a UART RX IRQ). `state` is passed back
// verbatim to each callback.
typedef struct {
    int  (*getc)(void *state);
    void (*putc)(void *state, int c);
    int  (*poll)(void *state);
    void *state;
} gdb_transport_t;

// Install a transport as the active GDB channel, replacing the default COM2. The
// descriptor is copied, so the caller need not keep it alive.
void gdb_register_transport(const gdb_transport_t *transport);

// Revert to the built-in COM2 channel (e.g. when a USB-serial transport is
// unplugged). If `transport` is non-NULL the revert only happens when it is
// still the active transport; pass NULL to force it.
void gdb_unregister_transport(const gdb_transport_t *transport);

// Poll the active transport for async break-in (a connecting GDB or a Ctrl-C
// sent to halt a running target) and, if a byte arrived, break into the stub.
// A no-op when the active transport has no `poll`. Returns 0. A transport with
// no receive IRQ drives this from a polling loop; the break-in decision and the
// entire RSP protocol stay owned by SysGdb -- the caller needs no GDB knowledge.
int gdb_poll_breakin(void);

#endif
