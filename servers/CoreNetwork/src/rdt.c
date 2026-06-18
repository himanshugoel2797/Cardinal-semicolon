// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "checksum.h"
#include "CoreNetwork/udp.h"
#include "CoreNetwork/rdt.h"

// Reliable Delivery Transport. See servers/inc/CoreNetwork/rdt.h for the wire
// protocol. All state below is protected by rdt_lock; the lock is dropped before
// any udp_send_to or completion callback (which must not run under a lock, and
// which may re-enter the network stack). Stop-and-wait on the host side means
// the same transfer is never in flight twice, so a snapshot-then-act structure
// is race-free for a given transfer.

#define RDT_MAX_LISTENERS 4
#define RDT_MAX_XFERS 4
#define RDT_MEMO 8

typedef struct {
    bool used;
    uint16_t port;
    rdt_complete_t on_complete;
    void *ctx;
} rdt_listener_t;

typedef struct {
    bool used;
    uint32_t src_ip;     // network order
    uint16_t src_port;   // host order
    uint16_t port;       // our listener port (host order)
    uint32_t xfer_id;
    uint8_t *buf;
    uint32_t total_len;
    uint32_t recd;
    char name[RDT_MAX_NAME];
    rdt_listener_t *listener;
} rdt_xfer_t;

// Records a recently-completed transfer so a retransmitted final DATA/START
// (after the buffer was freed) can be re-ACKed with FIN, idempotently, without
// re-running the sink.
typedef struct {
    bool used;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t xfer_id;
    uint32_t total_len;
} rdt_memo_t;

static rdt_listener_t rdt_listeners[RDT_MAX_LISTENERS];
static rdt_xfer_t rdt_xfers[RDT_MAX_XFERS];
static rdt_memo_t rdt_memos[RDT_MEMO];
static int rdt_xfer_rr;   // round-robin eviction cursor when the table is full
static int rdt_memo_rr;
static int rdt_lock = 0;

// --- checksum over header (excluding its own 2 csum bytes) + trailing ---------
// Returns the value to STORE in hdr->csum (network order via TO_BE_FRM_LE_16),
// NOT a fold-to-zero over a packet that already includes the field. To verify a
// received packet, recompute this and compare against TO_LE_FRM_BE_16(hdr->csum).
static uint16_t rdt_csum(const rdt_hdr_t *hdr, const void *trailing, int tlen) {
    // csum is the final field, so summing the first sizeof-2 bytes skips it.
    uint32_t s = net_csum_acc(0, hdr, (int)sizeof(rdt_hdr_t) - 2);
    if (trailing != NULL && tlen > 0)
        s = net_csum_acc(s, trailing, tlen);
    return net_csum_fold(s);
}

static bool rdt_magic_ok(const rdt_hdr_t *hdr) {
    return hdr->magic[0] == RDT_MAGIC0 && hdr->magic[1] == RDT_MAGIC1 &&
           hdr->magic[2] == RDT_MAGIC2 && hdr->magic[3] == RDT_MAGIC3;
}

// Build and transmit an ACK for `xfer_id` reporting `offset` cumulative bytes,
// from our `port` back to the sender. No state is held here.
static void rdt_send_ack(void *iface, uint32_t src_ip, const uint8_t *src_mac,
                         uint16_t port, uint16_t peer_port, uint32_t xfer_id,
                         uint32_t offset, bool fin) {
    rdt_hdr_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic[0] = RDT_MAGIC0;
    ack.magic[1] = RDT_MAGIC1;
    ack.magic[2] = RDT_MAGIC2;
    ack.magic[3] = RDT_MAGIC3;
    ack.type = RDT_TYPE_ACK;
    ack.flags = fin ? RDT_FLAG_FIN : 0;
    ack.xfer_id = TO_BE_FRM_LE_32(xfer_id);
    ack.offset = TO_BE_FRM_LE_32(offset);
    ack.csum = TO_BE_FRM_LE_16(rdt_csum(&ack, NULL, 0));

    udp_send_to(iface, src_ip, src_mac, port, peer_port, &ack, (int)sizeof(ack));
}

// --- memo (idempotent re-ACK after completion) -- caller holds rdt_lock --------
static rdt_memo_t *rdt_memo_find(uint32_t src_ip, uint16_t src_port, uint32_t xfer_id) {
    for (int i = 0; i < RDT_MEMO; i++) {
        if (rdt_memos[i].used && rdt_memos[i].src_ip == src_ip &&
            rdt_memos[i].src_port == src_port && rdt_memos[i].xfer_id == xfer_id)
            return &rdt_memos[i];
    }
    return NULL;
}

static void rdt_memo_add(uint32_t src_ip, uint16_t src_port, uint32_t xfer_id,
                         uint32_t total_len) {
    rdt_memo_t *m = rdt_memo_find(src_ip, src_port, xfer_id);
    if (m == NULL) {
        m = &rdt_memos[rdt_memo_rr];
        rdt_memo_rr = (rdt_memo_rr + 1) % RDT_MEMO;
    }
    m->used = true;
    m->src_ip = src_ip;
    m->src_port = src_port;
    m->xfer_id = xfer_id;
    m->total_len = total_len;
}

// --- transfer table -- caller holds rdt_lock ----------------------------------
static rdt_xfer_t *rdt_xfer_find(uint32_t src_ip, uint16_t src_port, uint32_t xfer_id) {
    for (int i = 0; i < RDT_MAX_XFERS; i++) {
        if (rdt_xfers[i].used && rdt_xfers[i].src_ip == src_ip &&
            rdt_xfers[i].src_port == src_port && rdt_xfers[i].xfer_id == xfer_id)
            return &rdt_xfers[i];
    }
    return NULL;
}

// Take a transfer slot, evicting the round-robin victim (freeing its buffer) if
// the table is full. Caller holds rdt_lock.
static rdt_xfer_t *rdt_xfer_alloc(void) {
    for (int i = 0; i < RDT_MAX_XFERS; i++)
        if (!rdt_xfers[i].used)
            return &rdt_xfers[i];

    // Table full: evict the round-robin victim. We deliberately do NOT memo it as
    // completed -- it was incomplete, its buffer is discarded, and its sink never
    // ran, so signalling FIN would be a false success. The victim's host instead
    // sees its next DATA dropped, times out, and restarts. (Concurrent transfers
    // are a pathological case for this stop-and-wait debug transport; in practice
    // the host runs one at a time.)
    rdt_xfer_t *victim = &rdt_xfers[rdt_xfer_rr];
    rdt_xfer_rr = (rdt_xfer_rr + 1) % RDT_MAX_XFERS;
    if (victim->buf != NULL) {
        free(victim->buf);
        victim->buf = NULL;
    }
    victim->used = false;
    return victim;
}

static void rdt_handle_start(rdt_listener_t *l, void *iface, uint32_t src_ip,
                             const uint8_t *src_mac, uint16_t src_port,
                             const rdt_hdr_t *hdr, const uint8_t *trailing,
                             int tlen) {
    uint32_t xfer_id = TO_LE_FRM_BE_32(hdr->xfer_id);
    uint32_t total_len = TO_LE_FRM_BE_32(hdr->total_len);
    uint16_t name_len = TO_LE_FRM_BE_16(hdr->name_len);

    if (total_len > RDT_MAX_BLOB)
        return;  // hostile/oversized -> drop, host will give up
    if (name_len >= RDT_MAX_NAME || (int)name_len > tlen)
        return;  // name doesn't fit / not fully present

    bool do_ack = false;
    uint32_t ack_off = 0;

    local_spinlock_lock(&rdt_lock);
    rdt_xfer_t *x = rdt_xfer_find(src_ip, src_port, xfer_id);
    if (x != NULL) {
        // Duplicate START (our ACK was lost) -> re-ACK current progress.
        do_ack = true;
        ack_off = x->recd;
    } else {
        rdt_memo_t *m = rdt_memo_find(src_ip, src_port, xfer_id);
        if (m != NULL) {
            // Already completed -> re-ACK FIN below (handled after unlock).
            ack_off = m->total_len;
        } else {
            x = rdt_xfer_alloc();
            uint8_t *buf = (total_len > 0) ? (uint8_t *)malloc(total_len) : NULL;
            if (total_len > 0 && buf == NULL) {
                local_spinlock_unlock(&rdt_lock);
                return;  // out of memory -> drop
            }
            x->used = true;
            x->src_ip = src_ip;
            x->src_port = src_port;
            x->port = l->port;
            x->xfer_id = xfer_id;
            x->buf = buf;
            x->total_len = total_len;
            x->recd = 0;
            x->listener = l;
            memcpy(x->name, trailing, name_len);
            x->name[name_len] = '\0';
            do_ack = true;
            ack_off = 0;
        }
    }

    // A zero-length blob is complete the moment it starts.
    bool complete = false;
    uint8_t *cbuf = NULL;
    char cname[RDT_MAX_NAME];
    rdt_complete_t cb = NULL;
    void *cb_ctx = NULL;
    if (x != NULL && x->used && x->recd == x->total_len) {
        complete = true;
        cbuf = x->buf;
        memcpy(cname, x->name, sizeof(cname));
        cb = x->listener->on_complete;
        cb_ctx = x->listener->ctx;
        rdt_memo_add(src_ip, src_port, xfer_id, x->total_len);
        x->used = false;
        x->buf = NULL;
    }
    local_spinlock_unlock(&rdt_lock);

    if (complete) {
        // Reached only by a zero-length blob (total_len == 0); offset/len are 0,
        // but pass total_len so this matches the DATA completion path's ACK.
        if (cb != NULL)
            cb(cb_ctx, iface, src_ip, src_mac, src_port, cname, cbuf, (int)total_len);
        if (cbuf != NULL)
            free(cbuf);
        rdt_send_ack(iface, src_ip, src_mac, l->port, src_port, xfer_id, total_len, true);
        return;
    }

    if (do_ack)
        rdt_send_ack(iface, src_ip, src_mac, l->port, src_port, xfer_id, ack_off, false);
    else  // memo hit: completed earlier
        rdt_send_ack(iface, src_ip, src_mac, l->port, src_port, xfer_id, ack_off, true);
}

static void rdt_handle_data(rdt_listener_t *l, void *iface, uint32_t src_ip,
                            const uint8_t *src_mac, uint16_t src_port,
                            const rdt_hdr_t *hdr, const uint8_t *trailing,
                            int tlen) {
    uint32_t xfer_id = TO_LE_FRM_BE_32(hdr->xfer_id);
    uint32_t offset = TO_LE_FRM_BE_32(hdr->offset);
    uint16_t chunk_len = TO_LE_FRM_BE_16(hdr->chunk_len);

    if ((int)chunk_len > tlen)
        return;  // chunk not fully present -> drop

    bool do_ack = false, fin = false, complete = false;
    uint32_t ack_off = 0;
    uint8_t *cbuf = NULL;
    char cname[RDT_MAX_NAME];
    uint32_t clen = 0;
    rdt_complete_t cb = NULL;
    void *cb_ctx = NULL;

    local_spinlock_lock(&rdt_lock);
    rdt_xfer_t *x = rdt_xfer_find(src_ip, src_port, xfer_id);
    if (x == NULL) {
        rdt_memo_t *m = rdt_memo_find(src_ip, src_port, xfer_id);
        if (m != NULL) {            // already completed -> re-ACK FIN
            do_ack = true;
            fin = true;
            ack_off = m->total_len;
        }
        // else: no START seen -> drop (host will restart on timeout)
    } else if (offset != x->recd) {
        // Duplicate or out-of-order: re-ACK what we actually have.
        do_ack = true;
        ack_off = x->recd;
    } else if ((uint64_t)offset + chunk_len > x->total_len) {
        // Would overrun the declared length: re-ACK current progress, no copy.
        do_ack = true;
        ack_off = x->recd;
    } else {
        if (chunk_len > 0)
            memcpy(x->buf + offset, trailing, chunk_len);
        x->recd += chunk_len;
        do_ack = true;
        ack_off = x->recd;

        if (x->recd == x->total_len) {
            complete = true;
            fin = true;
            cbuf = x->buf;
            clen = x->total_len;
            memcpy(cname, x->name, sizeof(cname));
            cb = x->listener->on_complete;
            cb_ctx = x->listener->ctx;
            rdt_memo_add(src_ip, src_port, xfer_id, x->total_len);
            x->used = false;
            x->buf = NULL;
        }
    }
    local_spinlock_unlock(&rdt_lock);

    if (complete && cb != NULL)
        cb(cb_ctx, iface, src_ip, src_mac, src_port, cname, cbuf, (int)clen);
    if (complete && cbuf != NULL)
        free(cbuf);

    if (do_ack)
        rdt_send_ack(iface, src_ip, src_mac, l->port, src_port, xfer_id, ack_off, fin);
}

// UDP handler bound to an RDT listener port. ctx is the rdt_listener_t.
static void rdt_udp_handler(void *ctx, void *iface, uint32_t src_ip,
                            const uint8_t *src_mac, uint16_t src_port,
                            uint16_t dst_port, const void *payload, int len) {
    (void)dst_port;
    rdt_listener_t *l = (rdt_listener_t *)ctx;

    if (len < (int)sizeof(rdt_hdr_t))
        return;
    const rdt_hdr_t *hdr = (const rdt_hdr_t *)payload;
    if (!rdt_magic_ok(hdr))
        return;

    const uint8_t *trailing = (const uint8_t *)payload + sizeof(rdt_hdr_t);
    int tlen = len - (int)sizeof(rdt_hdr_t);

    if (rdt_csum(hdr, trailing, tlen) != TO_LE_FRM_BE_16(hdr->csum))
        return;  // corrupt -> drop

    switch (hdr->type) {
        case RDT_TYPE_START:
            rdt_handle_start(l, iface, src_ip, src_mac, src_port, hdr, trailing, tlen);
            break;
        case RDT_TYPE_DATA:
            rdt_handle_data(l, iface, src_ip, src_mac, src_port, hdr, trailing, tlen);
            break;
        default:
            break;  // ACK is device->host; ignore anything else
    }
}

int rdt_listen(uint16_t port, rdt_complete_t on_complete, void *ctx) {
    if (port == 0 || on_complete == NULL)
        return -1;

    local_spinlock_lock(&rdt_lock);
    rdt_listener_t *slot = NULL;
    for (int i = 0; i < RDT_MAX_LISTENERS; i++) {
        if (!rdt_listeners[i].used) {
            slot = &rdt_listeners[i];
            break;
        }
    }
    if (slot == NULL) {
        local_spinlock_unlock(&rdt_lock);
        return -1;
    }
    slot->used = true;
    slot->port = port;
    slot->on_complete = on_complete;
    slot->ctx = ctx;
    local_spinlock_unlock(&rdt_lock);

    // udp_bind is called with the lock released (it takes its own lock). The
    // brief window where `slot` is marked used before the bind confirms is benign:
    // rdt_listen is only called at module init, not concurrently.
    if (udp_bind(port, rdt_udp_handler, slot) != 0) {
        local_spinlock_lock(&rdt_lock);
        slot->used = false;
        local_spinlock_unlock(&rdt_lock);
        return -1;
    }
    return 0;
}
