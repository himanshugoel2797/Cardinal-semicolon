// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// CSMUX framing layer over COM1. See SysDebug/csmux.h for the protocol and the
// rationale. This file owns the raw COM1 byte I/O used in framed mode; the
// unframed path stays in serialio.c and is only displaced once csmux_activate()
// flips g_active.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "SysDebug/csmux.h"

static volatile bool g_active = false;
static int g_tx_lock = 0; // serialises whole-frame sends across cores
static int g_rx_lock = 0; // serialises the receive de-framer across cores

// --- console UART target (busy-polled; safe in trap/cli context) -------------
//
// The defaults reproduce the original hardcoded COM1 path exactly: legacy port
// I/O at 0x3f8, byte-spaced registers. So early boot (before ACPI SPCR is
// parsed) and platforms without an SPCR table (e.g. QEMU) are unchanged.
// sysdebug_console_uart_set() retargets these once SPCR names the real console
// UART -- e.g. the AtomicPi's MMIO DesignWare 8250 (32-bit registers, reg << 2).
static volatile uintptr_t g_uart_base = 0x3f8; // I/O port, or virtual MMIO base
static volatile int g_uart_mmio = 0;           // 0 = port I/O, 1 = MMIO
static volatile int g_uart_shift = 0;          // reg N at base + (N << shift)
static volatile int g_uart_mmio32 = 0;         // MMIO width: 1 = 32-bit, else 8-bit

#define UART_REG_DATA 0     // RBR (read) / THR (write) / DLL (low divisor, DLAB=1)
#define UART_REG_IER 1      // interrupt enable / DLM (high divisor, DLAB=1)
#define UART_REG_FCR 2      // FIFO control (write)
#define UART_REG_LCR 3      // line control
#define UART_REG_MCR 4      // modem control
#define UART_REG_LSR 5      // line status register
#define UART_LSR_THRE 0x20  // TX holding register empty
#define UART_LSR_DR 0x01    // RX data ready
#define UART_LCR_DLAB 0x80  // divisor-latch access
#define UART_LCR_8N1 0x03   // 8 data bits, no parity, 1 stop

static inline uint8_t uart_reg_rd(int reg) {
    uintptr_t a = g_uart_base + ((uintptr_t)reg << g_uart_shift);
    if (!g_uart_mmio)
        return inb((uint16_t)a);
    if (g_uart_mmio32)
        return (uint8_t)*(volatile uint32_t *)a;
    return *(volatile uint8_t *)a;
}

static inline void uart_reg_wr(int reg, uint8_t v) {
    uintptr_t a = g_uart_base + ((uintptr_t)reg << g_uart_shift);
    if (!g_uart_mmio)
        outb((uint16_t)a, v);
    else if (g_uart_mmio32)
        *(volatile uint32_t *)a = v;
    else
        *(volatile uint8_t *)a = v;
}

void csmux_uart_putb(uint8_t b) {
    while ((uart_reg_rd(UART_REG_LSR) & UART_LSR_THRE) == 0)
        ;
    uart_reg_wr(UART_REG_DATA, b);
}

// Non-blocking: returns the next byte or -1 if the RX FIFO is empty.
int csmux_uart_getb(void) {
    if ((uart_reg_rd(UART_REG_LSR) & UART_LSR_DR) == 0)
        return -1;
    return uart_reg_rd(UART_REG_DATA);
}

void csmux_uart_init(void) {
    // Program the console UART for 115200 8N1, FIFOs on, polled (no IRQ). The
    // firmware/GRUB leave it configured, but the multiboot2 hand-off can reset
    // the divisor on real hardware -- observed on the AtomicPi, whose serial then
    // came out at the wrong baud (a corrupt stream over the link). Set it
    // explicitly rather than inheriting an unknown rate. Divisor 1 = 115200 on the
    // standard 1.8432 MHz 16550 clock (legacy COM1). For an MMIO/LPSS UART on a
    // different clock this divisor would need recomputing -- not done here, since
    // the console stays on the legacy 0x3f8 port.
    uart_reg_wr(UART_REG_IER, 0x00);           // disable interrupts
    uart_reg_wr(UART_REG_LCR, UART_LCR_DLAB);  // DLAB=1: data/IER become DLL/DLM
    uart_reg_wr(UART_REG_DATA, 0x01);          // DLL = 1  -> 115200
    uart_reg_wr(UART_REG_IER, 0x00);           // DLM = 0
    uart_reg_wr(UART_REG_LCR, UART_LCR_8N1);   // 8N1, DLAB=0
    uart_reg_wr(UART_REG_FCR, 0xC7);           // enable + clear FIFOs, 14B trigger
    uart_reg_wr(UART_REG_MCR, 0x03);           // DTR | RTS (no loopback)
}

void sysdebug_console_uart_set(int is_mmio, uint64_t base, int reg_shift, int access32) {
    // Swing the target under both link locks so no core is mid-write or mid-pump
    // on the old UART when the pointers change.
    int if_state = cli();
    local_spinlock_lock(&g_tx_lock);
    local_spinlock_lock(&g_rx_lock);
    g_uart_base = (uintptr_t)base;
    g_uart_mmio = is_mmio ? 1 : 0;
    g_uart_shift = reg_shift;
    g_uart_mmio32 = access32 ? 1 : 0;
    local_spinlock_unlock(&g_rx_lock);
    local_spinlock_unlock(&g_tx_lock);
    sti(if_state);
}

// --- default byte transport: the polled console UART -------------------------

static void com1_write(void *state, const uint8_t *buf, uint32_t len) {
    (void)state;
    for (uint32_t i = 0; i < len; i++)
        csmux_uart_putb(buf[i]);
}

static int com1_getb_xport(void *state) {
    (void)state;
    return csmux_uart_getb();
}

static csmux_transport_t g_xport = {com1_write, com1_getb_xport, NULL};

// A "heavy" transport (e.g. FTDI: write == a USB bulk transfer) may itself emit
// to the debug log on its error paths, which would re-enter csmux_send. To avoid
// self-deadlock on g_tx_lock we track the lock owner (by initial APIC id) and
// drop a re-entrant frame. COM1 (the default) never re-enters, so the guard is
// only armed once a custom transport is installed -- keeping the common path free
// of the cpuid cost.
static bool g_xport_heavy = false;
static volatile int g_tx_owner = -1;

static inline int csmux_cpu(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (int)(b >> 24); // initial APIC id
}

void csmux_set_transport(const csmux_transport_t *t) {
    if (t == NULL || t->write == NULL || t->getb == NULL)
        return;
    int if_state = cli();
    local_spinlock_lock(&g_tx_lock);
    local_spinlock_lock(&g_rx_lock);
    g_xport = *t;
    g_xport_heavy = true;
    local_spinlock_unlock(&g_rx_lock);
    local_spinlock_unlock(&g_tx_lock);
    sti(if_state);
}

// Acquire the TX lock. Returns 1 to proceed (caller must tx_release), or 0 if the
// call is a same-core re-entry on a heavy transport and must be dropped.
static int tx_acquire(int *if_state) {
    *if_state = cli();
    if (g_xport_heavy) {
        int me = csmux_cpu();
        if (g_tx_owner == me) { // re-entry (log from inside the transport write)
            sti(*if_state);
            return 0;
        }
        local_spinlock_lock(&g_tx_lock);
        g_tx_owner = me;
    } else {
        local_spinlock_lock(&g_tx_lock);
    }
    return 1;
}

static void tx_release(int if_state) {
    if (g_xport_heavy)
        g_tx_owner = -1;
    local_spinlock_unlock(&g_tx_lock);
    sti(if_state);
}

// --- CRC16-CCITT (poly 0x1021, init 0xFFFF), bitwise: no table, trap-safe ----

static uint16_t crc16_upd(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

// --- TX ----------------------------------------------------------------------

// Whole-frame staging buffer (byte-stuffed). Worst case is every body byte
// stuffed to two, plus the two SOF/EOF delimiters. Written only under g_tx_lock,
// so a single static buffer is safe and keeps this off the (small) trap stack.
#define CSMUX_FRAME_MAX (2u * (3u + CSMUX_MAX_PAYLOAD + 2u) + 2u)
static uint8_t g_stage[CSMUX_FRAME_MAX];

static uint32_t stuff_into(uint8_t *out, uint32_t o, uint8_t b) {
    if (b == 0x7E || b == 0x7D) {
        out[o++] = 0x7D;
        out[o++] = (uint8_t)(b ^ 0x20);
    } else {
        out[o++] = b;
    }
    return o;
}

// Build + emit one frame to the transport. Caller must hold the TX lock.
static void emit_frame_locked(uint8_t chan, const uint8_t *p, uint32_t len) {
    uint8_t hdr[3] = {chan, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)};
    uint16_t crc = 0xFFFF;
    uint32_t o = 0;
    g_stage[o++] = 0x7E;
    for (int i = 0; i < 3; i++) {
        o = stuff_into(g_stage, o, hdr[i]);
        crc = crc16_upd(crc, hdr[i]);
    }
    for (uint32_t i = 0; i < len; i++) {
        o = stuff_into(g_stage, o, p[i]);
        crc = crc16_upd(crc, p[i]);
    }
    o = stuff_into(g_stage, o, (uint8_t)(crc & 0xFF));
    o = stuff_into(g_stage, o, (uint8_t)((crc >> 8) & 0xFF));
    g_stage[o++] = 0x7E;
    // One transport write per frame: the FTDI transport turns this into a single
    // USB bulk batch rather than a (disastrous) transfer per byte, and one write
    // keeps the frame atomic.
    g_xport.write(g_xport.state, g_stage, o);
}

// Coalesced log buffer. The debug log is high-volume; emitting a CSMUX_CH_LOG
// frame per print_str would be one USB transfer per line over an FTDI link, whose
// per-transfer latency stalls the low-rate control/GDB channels sharing the link.
// Instead we accumulate log bytes and flush them as a few large CH_LOG frames.
static uint8_t g_log_buf[CSMUX_MAX_PAYLOAD];
static uint32_t g_log_len = 0;

static void flush_log_locked(void) {
    if (g_log_len > 0) {
        emit_frame_locked(CSMUX_CH_LOG, g_log_buf, g_log_len);
        g_log_len = 0;
    }
}

int csmux_send(uint8_t chan, const void *buf, uint32_t len) {
    if (!g_active)
        return -1;
    if (len > CSMUX_MAX_PAYLOAD)
        return -1;
    int if_state;
    if (!tx_acquire(&if_state))
        return -2; // re-entrant on a heavy transport: drop to avoid self-deadlock
    // Flush any buffered log first so control/GDB frames keep order with the log
    // and are never queued behind it.
    if (chan != CSMUX_CH_LOG)
        flush_log_locked();
    emit_frame_locked(chan, (const uint8_t *)buf, len);
    tx_release(if_state);
    return 0;
}

void csmux_log_append(const void *buf, uint32_t len) {
    if (!g_active)
        return;
    const uint8_t *p = (const uint8_t *)buf;
    int if_state;
    if (!tx_acquire(&if_state))
        return; // re-entrant (log from inside a transport write): drop
    for (uint32_t i = 0; i < len; i++) {
        if (g_log_len >= CSMUX_MAX_PAYLOAD)
            flush_log_locked();
        g_log_buf[g_log_len++] = p[i];
    }
    tx_release(if_state);
}

void csmux_log_flush(void) {
    if (!g_active)
        return;
    int if_state;
    if (!tx_acquire(&if_state))
        return;
    flush_log_locked();
    tx_release(if_state);
}

void csmux_raw_write(const void *buf, uint32_t len) {
    // Unframed write -- bypasses the pluggable transport (never the possibly-USB
    // link), going straight to the current console UART (PIO or, after an SPCR
    // retarget, MMIO). This is the pre-activation boot log, which can be emitted
    // from inside the USB stack itself, so routing it over a USB transport could
    // re-enter the USB layer. Held under g_tx_lock so a core still logging raw cannot interleave
    // its bytes with another core's COM1 frame (which corrupted the handshake
    // frame intermittently when the transport is COM1).
    int if_state = cli();
    local_spinlock_lock(&g_tx_lock);
    com1_write(NULL, (const uint8_t *)buf, len);
    local_spinlock_unlock(&g_tx_lock);
    sti(if_state);
}

// --- RX rings (one per inbound channel) --------------------------------------

#define RING_CAP 1024u
// A full-size frame's payload must fit a receive ring with room to spare.
_Static_assert(RING_CAP >= CSMUX_MAX_PAYLOAD, "RING_CAP must hold a max payload");

typedef struct {
    uint8_t buf[RING_CAP];
    volatile uint32_t head; // producer (pump)
    volatile uint32_t tail; // consumer (reader)
} ring_t;

// g_rx_lock (declared at top) serialises the receive de-framer:
// csmux_recv_byte_pump mutates shared deframer state (g_frame/g_flen/g_inframe/
// g_escape) and the ring heads, so two cores pumping at once (e.g. the SysTest
// control reader on one core and the tunneled GDB reader on another) would
// corrupt it. Distinct from g_tx_lock and never nested with it (the pump never
// sends, csmux_send never pumps), so no deadlock.

static ring_t g_ctrl_ring;
static ring_t g_gdb_ring;

static ring_t *ring_for(uint8_t chan) {
    if (chan == CSMUX_CH_CTRL)
        return &g_ctrl_ring;
    if (chan == CSMUX_CH_GDB)
        return &g_gdb_ring;
    return NULL; // CH_LOG (or unknown) has no inbound ring
}

static void ring_push(ring_t *r, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t nh = (r->head + 1) % RING_CAP;
        if (nh == r->tail)
            return; // full: drop the rest
        r->buf[r->head] = src[i];
        r->head = nh;
    }
}

static uint32_t ring_avail(const ring_t *r) {
    return (r->head + RING_CAP - r->tail) % RING_CAP;
}

static uint32_t ring_drain(ring_t *r, uint8_t *dst, uint32_t cap) {
    uint32_t n = 0;
    while (n < cap && r->tail != r->head) {
        dst[n++] = r->buf[r->tail];
        r->tail = (r->tail + 1) % RING_CAP;
    }
    return n;
}

// --- RX de-framer ------------------------------------------------------------

static uint8_t g_frame[3 + CSMUX_MAX_PAYLOAD + 2];
static uint32_t g_flen = 0;
static bool g_inframe = false;
static bool g_escape = false;

static void handle_frame(void) {
    uint32_t n = g_flen;
    if (n < 5)
        return; // need at least chan + len(2) + crc(2)
    uint8_t chan = g_frame[0];
    uint32_t len = (uint32_t)g_frame[1] | ((uint32_t)g_frame[2] << 8);
    if (len != n - 5)
        return; // length mismatch -> drop
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < 3 + len; i++)
        crc = crc16_upd(crc, g_frame[i]);
    uint16_t fcrc = (uint16_t)g_frame[3 + len] | ((uint16_t)g_frame[3 + len + 1] << 8);
    if (crc != fcrc)
        return; // bad CRC -> drop
    ring_t *r = ring_for(chan);
    if (r != NULL)
        ring_push(r, &g_frame[3], len);
}

int csmux_recv_byte_pump(void) {
    int n = 0;
    int b;
    int if_state = cli();
    local_spinlock_lock(&g_rx_lock);
    while ((b = g_xport.getb(g_xport.state)) >= 0) {
        n++;
        uint8_t c = (uint8_t)b;
        if (c == 0x7E) {
            if (g_inframe && g_flen >= 5)
                handle_frame();
            g_inframe = true;
            g_flen = 0;
            g_escape = false;
            continue;
        }
        if (!g_inframe)
            continue; // junk between frames (e.g. boot text) -> ignore
        if (c == 0x7D) {
            g_escape = true;
            continue;
        }
        if (g_escape) {
            c = (uint8_t)(c ^ 0x20);
            g_escape = false;
        }
        if (g_flen < sizeof(g_frame))
            g_frame[g_flen++] = c;
        else
            g_inframe = false; // overflow -> drop, resync on next 0x7E
    }
    local_spinlock_unlock(&g_rx_lock);
    sti(if_state);
    return n;
}

int csmux_chan_read(uint8_t chan, void *buf, uint32_t cap) {
    csmux_recv_byte_pump();
    ring_t *r = ring_for(chan);
    if (r == NULL)
        return 0;
    return (int)ring_drain(r, (uint8_t *)buf, cap);
}

int csmux_chan_avail(uint8_t chan) {
    csmux_recv_byte_pump();
    ring_t *r = ring_for(chan);
    if (r == NULL)
        return 0;
    return (int)ring_avail(r);
}

// --- activation --------------------------------------------------------------

bool csmux_active(void) { return g_active; }
bool csmux_xport_heavy(void) { return g_xport_heavy; }

void csmux_activate(void) {
    if (g_active)
        return;
    // Raw banner over the active link: gives the host demuxer an unambiguous
    // raw->framed boundary (and a re-sync marker after every reboot, since the
    // banner reprints). Sent via the transport so it rides COM1 or FTDI alike.
    static const char banner[] = "\r\n[[CSMUX-START v1]]\r\n";
    int if_state;
    if (tx_acquire(&if_state)) {
        if (!g_active) { // re-check under the lock so the banner is emitted once
            g_xport.write(g_xport.state, (const uint8_t *)banner, (uint32_t)(sizeof(banner) - 1));
            g_active = true; // set inside the lock: no window where banner sent but flag clear
        }
        tx_release(if_state);
    }
}
