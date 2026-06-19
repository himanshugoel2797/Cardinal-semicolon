// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSDEBUG_CSMUX_H
#define CARDINAL_SYSDEBUG_CSMUX_H

// CSMUX -- a tiny HDLC-style framing layer that multiplexes several logical
// channels over the single COM1 serial link. It exists so the in-OS test
// harness can have a private control channel and the interactive Lisp REPL can
// run while the human-readable debug log keeps flowing -- all over one wire (the
// case that matters on real hardware, where there may be only one USB-serial
// link).
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
#define CSMUX_CH_CTRL 1u // auxiliary output/log channel (no inbound ring) -- the
                         // REPL subsumed what the old control protocol drove
#define CSMUX_CH_REPL 2u // the interactive Lisp REPL (replaced the GDB tunnel)

#define CSMUX_MAX_PAYLOAD 1024u

// Pluggable byte transport. CSMUX defaults to polled COM1, but on real hardware
// the only link may be a USB-serial (FTDI) adapter; usb_serial registers itself
// as the transport so the whole mux rides that one link. `write` must deliver the
// whole frame atomically (one call) and `getb` returns the next received byte or
// -1 if none (non-blocking). NEITHER callback may emit to the debug log
// (print_str), or it would re-enter csmux_send under its own lock -- the COM1 and
// FTDI transports honour this (the USB transfer path takes no logging under its
// controller lock).
typedef struct {
    void (*write)(void *state, const uint8_t *buf, uint32_t len);
    int (*getb)(void *state); // next byte, or -1 if none (non-blocking)
    void *state;
} csmux_transport_t;

// Replace the active byte transport (default: COM1). Call before csmux_activate
// (e.g. from a USB-serial driver's probe). The descriptor is copied.
void csmux_set_transport(const csmux_transport_t *t);

// Switch the active link into framed mode. Emits a raw "[[CSMUX-START v1]]"
// banner first so the host demuxer has an unambiguous raw->framed sync point (and
// re-sync after each reboot). Idempotent. After this, print_str routes the log
// onto CH_LOG.
void csmux_activate(void);
bool csmux_active(void);

// True once a custom (non-default-COM1) transport has been installed -- i.e. a
// USB-serial adapter has been bound as the link. SysTest uses this to wait for an
// FTDI link to enumerate before the handshake, so the mux rides that single link.
bool csmux_xport_heavy(void);

// Append debug-log bytes to the coalescing buffer (CSMUX_CH_LOG). The log is
// high-volume, so instead of a frame (a USB transfer) per line it is batched and
// flushed as a few large frames -- otherwise per-line transfer latency over a USB
// link starves the low-rate control/REPL channels sharing the single link. The
// buffer auto-flushes when full, before any control/REPL frame, and via
// csmux_log_flush(). print_str routes the log here once CSMUX is active.
void csmux_log_append(const void *buf, uint32_t len);

// Flush any buffered log now (e.g. periodically, before idling).
void csmux_log_flush(void);

// Send one whole frame on `chan`. Emitted atomically with respect to other
// senders and safe to call from cli()/trap context (busy-polled TX, no IRQ/DMA,
// no malloc). Returns 0, or <0 if inactive or len > CSMUX_MAX_PAYLOAD.
//
// Re-entrancy caveat: csmux_send takes a TX spinlock under cli(). A CPU exception
// taken WHILE this core already holds that lock (i.e. a fault from inside a
// csmux_send / print_str frame) would self-deadlock if it tried to send again, so
// a fault/PANIC path must not re-enter csmux_send while the TX lock is held.
int csmux_send(uint8_t chan, const void *buf, uint32_t len);

// Unframed write over the active link, serialised by the same lock as
// csmux_send. print_str's raw (pre-activation) path uses this so raw logging from
// one core cannot interleave with another core's frame (which would corrupt it).
void csmux_raw_write(const void *buf, uint32_t len);

// Pump the COM1 receiver: read any bytes currently available, run the
// de-framer, and route complete frames into per-channel receive rings.
// Non-blocking. Returns the number of raw bytes consumed.
int csmux_recv_byte_pump(void);

// Drain up to `cap` already-received bytes on `chan` into `buf` (pumps first).
// Non-blocking; returns the number of bytes copied (0 if none). Only CH_REPL has
// a receive ring; CH_LOG and CH_CTRL are output-only (always return 0).
int csmux_chan_read(uint8_t chan, void *buf, uint32_t cap);

// Number of bytes currently waiting on `chan` (pumps first), without consuming
// them. Used by the REPL's non-blocking input poll.
int csmux_chan_avail(uint8_t chan);

#endif
