// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSDEBUG_CSMUX_H
#define CARDINAL_SYSDEBUG_CSMUX_H

// CSMUX -- a tiny HDLC-style framing layer that multiplexes several logical
// channels over the single COM1 serial link. It exists so the in-OS test
// harness can have a private control channel and a tunneled GDB channel while
// the human-readable debug log keeps flowing -- all over one wire (the case that
// matters on real hardware, where there may be only one USB-serial link).
//
// CSMUX is dormant on a normal boot: print_str writes raw text to COM1 exactly
// as before. It is only switched on (csmux_activate) when the kernel is booted
// for a harness-driven test run; after that, print_str frames the debug log onto
// CSMUX_CH_LOG and the host demuxer splits the channels back apart.
//
// Frame: 0x7E | chan(1) | len(2 LE) | payload[len] | crc16(2 LE) | 0x7E
//   - body bytes (chan..crc) are byte-stuffed: 0x7E -> 0x7D 0x5E, 0x7D -> 0x7D 0x5D
//   - crc16 is CRC16-CCITT (poly 0x1021, init 0xFFFF) over chan|len|payload

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CSMUX_CH_LOG 0u  // human-readable debug log (print_str)
#define CSMUX_CH_CTRL 1u // test harness control protocol (text messages)
#define CSMUX_CH_GDB 2u  // tunneled GDB remote-serial-protocol bytes

#define CSMUX_MAX_PAYLOAD 1024u

// Switch COM1 into framed mode. Emits a raw "[[CSMUX-START v1]]" banner first so
// the host demuxer has an unambiguous raw->framed sync point (and re-sync after
// each reboot). Idempotent. After this, print_str routes the log onto CH_LOG.
void csmux_activate(void);
bool csmux_active(void);

// Send one whole frame on `chan`. Emitted atomically with respect to other
// senders and safe to call from cli()/trap context (busy-polled TX, no IRQ/DMA,
// no malloc). Returns 0, or <0 if inactive or len > CSMUX_MAX_PAYLOAD.
//
// Re-entrancy caveat: csmux_send takes a TX spinlock under cli(). A CPU exception
// taken WHILE this core already holds that lock (i.e. a fault from inside a
// csmux_send / print_str frame) and then trying to send again would self-
// deadlock. The death-test hooks call csmux_send from the fault/PANIC path, so a
// death-test body must not be invoked while the TX lock is held -- in practice it
// never is (test bodies run from the runner, not from inside print_str).
int csmux_send(uint8_t chan, const void *buf, uint32_t len);

// Pump the COM1 receiver: read any bytes currently available, run the
// de-framer, and route complete frames into per-channel receive rings.
// Non-blocking. Returns the number of raw bytes consumed.
int csmux_recv_byte_pump(void);

// Drain up to `cap` already-received bytes on `chan` into `buf` (pumps first).
// Non-blocking; returns the number of bytes copied (0 if none). Only CH_CTRL and
// CH_GDB have receive rings.
int csmux_chan_read(uint8_t chan, void *buf, uint32_t cap);

// Number of bytes currently waiting on `chan` (pumps first), without consuming
// them. Used by the GDB transport's non-destructive break-in poll.
int csmux_chan_avail(uint8_t chan);

// Install a hook invoked at the top of debug_handle_trap() (the PANIC path),
// before the interactive debug shell. Used by SysTest so a PANIC during a death
// test reports the death and resets instead of dropping into the shell. The hook
// returns normally when it decides not to act. Resolved by name from SysTest.
void debug_set_trap_hook(void (*hook)(void));

#endif
