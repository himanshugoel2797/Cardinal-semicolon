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

#define COM1 ((uint16_t)0x3f8)
#define COM1_LSR (COM1 + 5)

// --- raw COM1 byte I/O (busy-polled; safe in trap/cli context) ---------------

static inline void com1_putb(uint8_t b) {
    while ((inb(COM1_LSR) & 0x20) == 0)
        ;
    outb(COM1, b);
}

// Non-blocking: returns the next byte or -1 if the RX FIFO is empty.
static inline int com1_getb(void) {
    if ((inb(COM1_LSR) & 0x01) == 0)
        return -1;
    return inb(COM1);
}

static void com1_puts_raw(const char *s) {
    while (*s != 0)
        com1_putb((uint8_t)*s++);
}

// --- CRC16-CCITT (poly 0x1021, init 0xFFFF), bitwise: no table, trap-safe ----

static uint16_t crc16_upd(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

// --- TX ----------------------------------------------------------------------

static bool g_active = false;
static int g_tx_lock = 0;

static inline void put_stuffed(uint8_t b) {
    if (b == 0x7E || b == 0x7D) {
        com1_putb(0x7D);
        com1_putb((uint8_t)(b ^ 0x20));
    } else {
        com1_putb(b);
    }
}

int csmux_send(uint8_t chan, const void *buf, uint32_t len) {
    if (!g_active)
        return -1;
    if (len > CSMUX_MAX_PAYLOAD)
        return -1;

    const uint8_t *p = (const uint8_t *)buf;
    uint8_t hdr[3] = {chan, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)};
    uint16_t crc = 0xFFFF;

    int if_state = cli();
    local_spinlock_lock(&g_tx_lock);

    com1_putb(0x7E);
    for (int i = 0; i < 3; i++) {
        put_stuffed(hdr[i]);
        crc = crc16_upd(crc, hdr[i]);
    }
    for (uint32_t i = 0; i < len; i++) {
        put_stuffed(p[i]);
        crc = crc16_upd(crc, p[i]);
    }
    put_stuffed((uint8_t)(crc & 0xFF));
    put_stuffed((uint8_t)((crc >> 8) & 0xFF));
    com1_putb(0x7E);

    local_spinlock_unlock(&g_tx_lock);
    sti(if_state);
    return 0;
}

// --- RX rings (one per inbound channel) --------------------------------------

#define RING_CAP 1024u
typedef struct {
    uint8_t buf[RING_CAP];
    volatile uint32_t head; // producer (pump)
    volatile uint32_t tail; // consumer (reader)
} ring_t;

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
    while ((b = com1_getb()) >= 0) {
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

void csmux_activate(void) {
    if (g_active)
        return;
    // Raw banner: gives the host demuxer an unambiguous raw->framed boundary
    // (and a re-sync marker after every reboot, since the banner reprints).
    com1_puts_raw("\r\n[[CSMUX-START v1]]\r\n");
    g_active = true;
}
