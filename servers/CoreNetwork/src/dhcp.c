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
#include <SysTaskMgr/task.h>

#include "dhcp.h"
#include "net_priv.h"
#include "CoreNetwork/udp.h"

// See dhcp.h for the protocol overview. This file has two halves: pure helpers
// (build/parse/apply -- no I/O, unit-tested) and the async driver (a per-
// interface client task + the port-68 rx handler) that runs the state machine
// with retransmission and lease renewal.

static const uint8_t dhcp_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ===========================================================================
// Pure helpers
// ===========================================================================

int dhcp_build(uint8_t *buf, int buflen, uint8_t msg_type, const uint8_t *mac,
               uint32_t xid, uint32_t ciaddr, uint32_t requested_ip,
               uint32_t server_id, bool broadcast) {
    if (buflen < (int)sizeof(dhcp_msg_t))
        return -1;

    dhcp_msg_t *m = (dhcp_msg_t *)buf;
    memset(m, 0, sizeof(*m));
    m->op = DHCP_OP_BOOTREQUEST;
    m->htype = DHCP_HTYPE_ETHERNET;
    m->hlen = DHCP_HLEN_ETHERNET;
    m->xid = TO_BE_FRM_LE_32(xid);
    m->flags = broadcast ? TO_BE_FRM_LE_16((uint16_t)DHCP_FLAG_BROADCAST) : 0;
    m->ciaddr = ciaddr;  // already network order (or 0)
    memcpy(m->chaddr, mac, 6);
    m->cookie = TO_BE_FRM_LE_32(DHCP_MAGIC_COOKIE);

    int cap = buflen - (int)sizeof(dhcp_msg_t);
    uint8_t *o = m->options;
    int off = 0;

    // option 53: message type
    if (off + 3 > cap)
        return -1;
    o[off++] = DHCP_OPT_MSGTYPE;
    o[off++] = 1;
    o[off++] = msg_type;

    // option 50: requested IP (a selecting REQUEST)
    if (requested_ip != 0) {
        if (off + 6 > cap)
            return -1;
        o[off++] = DHCP_OPT_REQUESTED_IP;
        o[off++] = 4;
        memcpy(o + off, &requested_ip, 4);
        off += 4;
    }

    // option 54: server identifier (a selecting REQUEST)
    if (server_id != 0) {
        if (off + 6 > cap)
            return -1;
        o[off++] = DHCP_OPT_SERVER_ID;
        o[off++] = 4;
        memcpy(o + off, &server_id, 4);
        off += 4;
    }

    // option 55: parameters we want back
    {
        static const uint8_t params[] = {
            DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_LEASE
        };
        if (off + 2 + (int)sizeof(params) > cap)
            return -1;
        o[off++] = DHCP_OPT_PARAM_LIST;
        o[off++] = (uint8_t)sizeof(params);
        memcpy(o + off, params, sizeof(params));
        off += (int)sizeof(params);
    }

    // option 255: end
    if (off + 1 > cap)
        return -1;
    o[off++] = DHCP_OPT_END;

    return (int)sizeof(dhcp_msg_t) + off;
}

int dhcp_parse(const void *buf, int len, uint32_t xid, const uint8_t *mac,
               dhcp_result_t *out) {
    if (len < (int)sizeof(dhcp_msg_t))
        return -1;

    const dhcp_msg_t *m = (const dhcp_msg_t *)buf;
    if (m->op != DHCP_OP_BOOTREPLY)
        return -1;
    if (m->cookie != TO_BE_FRM_LE_32(DHCP_MAGIC_COOKIE))
        return -1;
    if (m->xid != TO_BE_FRM_LE_32(xid))
        return -1;
    if (memcmp(m->chaddr, mac, 6) != 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->yiaddr = m->yiaddr;

    // Walk the TLV options. Every length is bounded against optlen before use.
    const uint8_t *o = m->options;
    int optlen = len - (int)sizeof(dhcp_msg_t);
    int off = 0;
    bool have_type = false;
    while (off < optlen) {
        uint8_t code = o[off++];
        if (code == DHCP_OPT_END)
            break;
        if (code == DHCP_OPT_PAD)
            continue;
        if (off >= optlen)
            break;  // missing length byte
        uint8_t l = o[off++];
        if (off + (int)l > optlen)
            break;  // declared length runs past the buffer

        switch (code) {
            case DHCP_OPT_MSGTYPE:
                if (l >= 1) {
                    out->msg_type = o[off];
                    have_type = true;
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (l >= 4)
                    memcpy(&out->server_id, o + off, 4);
                break;
            case DHCP_OPT_SUBNET:
                if (l >= 4)
                    memcpy(&out->netmask, o + off, 4);
                break;
            case DHCP_OPT_ROUTER:
                if (l >= 4)
                    memcpy(&out->gateway, o + off, 4);  // first router
                break;
            case DHCP_OPT_DNS:
                if (l >= 4)
                    memcpy(&out->dns, o + off, 4);  // first server
                break;
            case DHCP_OPT_LEASE:
                if (l >= 4) {
                    uint32_t v;
                    memcpy(&v, o + off, 4);
                    out->lease_secs = TO_LE_FRM_BE_32(v);
                }
                break;
            default:
                break;
        }
        off += l;
    }
    return have_type ? 0 : -1;
}

void dhcp_apply(interface_def_t *iface, const dhcp_result_t *r) {
    iface->ip = r->yiaddr;
    iface->netmask = r->netmask;
    iface->gateway = r->gateway;
    iface->dns = r->dns;
}

// ===========================================================================
// Async driver: per-interface client task + rx handler
// ===========================================================================

#define DHCP_MAX_IFACES 4
#define DHCP_MAX_RETRY 5

typedef struct {
    bool used;
    interface_def_t *iface;
    cs_id task;
    uint32_t xid;
    int lock;
    volatile bool reply_valid;  // a reply for the current xid is waiting
    dhcp_result_t reply;
    uint8_t server_mac[6];      // L2 source of the reply -> unicast renew target
} dhcp_slot_t;

static dhcp_slot_t dhcp_slots[DHCP_MAX_IFACES];
static int dhcp_slots_lock = 0;
static bool dhcp_port_bound = false;

// A per-host transaction id with no RNG: mix the MAC with a monotonically
// advancing counter so successive transactions don't reuse an id (which would
// match stale replies).
static uint32_t dhcp_next_xid(const uint8_t *mac) {
    static uint32_t ctr = 0;
    uint32_t base = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                    ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    uint32_t c = (uint32_t)__sync_add_and_fetch(&ctr, 1);  // tasks may race
    return base ^ (c * 2654435761u);
}

static void dhcp_log_cfg(const dhcp_result_t *r) {
    char t[16];
    DEBUG_PRINT("[DHCP] bound ");
    for (int i = 0; i < 4; i++) {
        DEBUG_PRINT(itoa((r->yiaddr >> (8 * i)) & 0xff, t, 10));
        if (i < 3)
            DEBUG_PRINT(".");
    }
    DEBUG_PRINT(" gw ");
    for (int i = 0; i < 4; i++) {
        DEBUG_PRINT(itoa((r->gateway >> (8 * i)) & 0xff, t, 10));
        if (i < 3)
            DEBUG_PRINT(".");
    }
    DEBUG_PRINT(" mask ");
    for (int i = 0; i < 4; i++) {
        DEBUG_PRINT(itoa((r->netmask >> (8 * i)) & 0xff, t, 10));
        if (i < 3)
            DEBUG_PRINT(".");
    }
    DEBUG_PRINT(" lease ");
    uint32_t ls = r->lease_secs > 0x7fffffffu ? 0x7fffffffu : r->lease_secs;
    DEBUG_PRINT(itoa((int)ls, t, 10));
    DEBUG_PRINT("s\r\n");
}

// The udp handler bound to port 68. Runs in the driver's rx context: match the
// reply to a slot by interface + current xid, stash it for the task. No sleeping.
static void dhcp_rx(void *ctx, void *iface_v, uint32_t src_ip,
                    const uint8_t *src_mac, uint16_t src_port, uint16_t dst_port,
                    const void *payload, int len) {
    (void)ctx;
    (void)src_ip;
    (void)src_port;
    (void)dst_port;
    interface_def_t *iface = (interface_def_t *)iface_v;

    for (int i = 0; i < DHCP_MAX_IFACES; i++) {
        dhcp_slot_t *s = &dhcp_slots[i];
        if (!s->used || s->iface != iface)
            continue;

        dhcp_result_t r;
        if (dhcp_parse(payload, len, s->xid, iface->mac, &r) != 0)
            continue;  // not a reply for this slot's current transaction

        local_spinlock_lock(&s->lock);
        s->reply = r;
        memcpy(s->server_mac, src_mac, 6);
        s->reply_valid = true;
        local_spinlock_unlock(&s->lock);
        return;
    }
}

// Wait up to total_ns for a reply, polling the slot between short sleeps. If
// want_type != 0, only a reply of that type satisfies the wait (others are
// consumed and ignored); want_type == 0 accepts any reply. Returns true + fills
// out on success.
static bool dhcp_wait(dhcp_slot_t *s, uint8_t want_type, dhcp_result_t *out,
                      uint64_t total_ns) {
    cs_id self = task_current();
    uint64_t step = MS(50);
    uint64_t waited = 0;
    while (waited < total_ns) {
        task_sleep(self, step);
        waited += step;

        bool valid = false;
        dhcp_result_t r;
        local_spinlock_lock(&s->lock);
        if (s->reply_valid) {
            valid = true;
            r = s->reply;
            s->reply_valid = false;
        }
        local_spinlock_unlock(&s->lock);

        if (valid && (want_type == 0 || r.msg_type == want_type)) {
            *out = r;
            return true;
        }
    }
    return false;
}

// Per-try wait window with simple exponential backoff (1s, 2s, 4s, ... cap 8s).
static uint64_t dhcp_backoff(int tries) {
    uint64_t secs = (uint64_t)1 << (tries > 3 ? 3 : tries);
    return SEC(secs);
}

// Broadcast a freshly-built message of msg_type for this slot. Returns true on a
// successful send.
static bool dhcp_send(dhcp_slot_t *s, uint8_t msg_type, uint32_t ciaddr,
                      uint32_t requested_ip, uint32_t server_id, bool broadcast,
                      const uint8_t *dst_mac, uint32_t dst_ip) {
    uint8_t buf[576];
    int n = dhcp_build(buf, sizeof(buf), msg_type, s->iface->mac, s->xid, ciaddr,
                       requested_ip, server_id, broadcast);
    if (n < 0)
        return false;
    return udp_send_to(s->iface, dst_ip, dst_mac, DHCP_CLIENT_PORT,
                       DHCP_SERVER_PORT, buf, n) == 0;
}

static void dhcp_task(void *arg) {
    dhcp_slot_t *s = (dhcp_slot_t *)arg;
    interface_def_t *iface = s->iface;
    cs_id self = task_current();

    while (true) {
        // ---- INIT / SELECTING: broadcast DISCOVER, await OFFER ----
        iface->ip = 0;
        iface->netmask = iface->gateway = iface->dns = 0;
        s->xid = dhcp_next_xid(iface->mac);
        s->reply_valid = false;

        dhcp_result_t offer;
        bool got_offer = false;
        for (int t = 0; t < DHCP_MAX_RETRY && !got_offer; t++) {
            dhcp_send(s, DHCP_DISCOVER, 0, 0, 0, true, dhcp_broadcast_mac, 0xFFFFFFFFu);
            got_offer = dhcp_wait(s, DHCP_OFFER, &offer, dhcp_backoff(t));
        }
        if (!got_offer) {
            task_sleep(self, SEC(5));  // no server answered; retry the cycle
            continue;
        }

        // ---- REQUESTING: broadcast REQUEST(server-id, requested-ip), await ACK ----
        dhcp_result_t ack;
        bool got_ack = false;
        bool naked = false;
        s->xid = dhcp_next_xid(iface->mac);
        for (int t = 0; t < DHCP_MAX_RETRY && !got_ack && !naked; t++) {
            dhcp_send(s, DHCP_REQUEST, 0, offer.yiaddr, offer.server_id, true,
                      dhcp_broadcast_mac, 0xFFFFFFFFu);
            dhcp_result_t r;
            if (dhcp_wait(s, 0, &r, dhcp_backoff(t))) {
                if (r.msg_type == DHCP_ACK) {
                    ack = r;
                    got_ack = true;
                } else if (r.msg_type == DHCP_NAK) {
                    naked = true;  // server refused -> restart from DISCOVER
                }
            }
        }
        if (!got_ack) {
            if (!naked)
                task_sleep(self, SEC(2));
            continue;
        }

        // ---- BOUND ----
        dhcp_apply(iface, &ack);
        dhcp_log_cfg(&ack);
        uint8_t server_mac[6];
        local_spinlock_lock(&s->lock);
        memcpy(server_mac, s->server_mac, 6);
        local_spinlock_unlock(&s->lock);

        uint32_t lease = ack.lease_secs ? ack.lease_secs : 3600;
        uint32_t server_id = ack.server_id;

        // ---- RENEWING: at T1 (lease/2), unicast REQUEST to the server ----
        bool renewed = true;
        while (renewed) {
            task_sleep(self, SEC(lease / 2));

            renewed = false;
            naked = false;
            s->xid = dhcp_next_xid(iface->mac);
            for (int t = 0; t < DHCP_MAX_RETRY && !renewed && !naked; t++) {
                // Unicast to the server we learned the MAC of (no ARP needed).
                dhcp_send(s, DHCP_REQUEST, iface->ip, 0, 0, false, server_mac, server_id);
                dhcp_result_t r;
                if (dhcp_wait(s, 0, &r, dhcp_backoff(t))) {
                    if (r.msg_type == DHCP_ACK) {
                        dhcp_apply(iface, &r);
                        lease = r.lease_secs ? r.lease_secs : lease;
                        renewed = true;
                    } else if (r.msg_type == DHCP_NAK) {
                        naked = true;
                    }
                }
            }
            // Renewal failed (timeout or NAK): fall out of the loop and the outer
            // while restarts the whole DISCOVER cycle with ip cleared.
        }
    }
}

void dhcp_start(interface_def_t *iface) {
    local_spinlock_lock(&dhcp_slots_lock);

    if (!dhcp_port_bound) {
        // Bind the client port once, globally; dhcp_rx fans replies to the right
        // slot by interface. (Nested under dhcp_slots_lock -> udp's lock only;
        // never the reverse, so no deadlock.)
        if (udp_bind(DHCP_CLIENT_PORT, dhcp_rx, NULL) == 0)
            dhcp_port_bound = true;
    }

    dhcp_slot_t *s = NULL;
    for (int i = 0; i < DHCP_MAX_IFACES; i++) {
        if (!dhcp_slots[i].used) {
            s = &dhcp_slots[i];
            break;
        }
    }
    if (s == NULL || !dhcp_port_bound) {
        local_spinlock_unlock(&dhcp_slots_lock);
        DEBUG_PRINT("[DHCP] cannot start (no slot or port bind failed)\r\n");
        return;
    }
    s->used = true;
    s->iface = iface;
    s->xid = 0;
    s->lock = 0;
    s->reply_valid = false;
    local_spinlock_unlock(&dhcp_slots_lock);

    cs_id id;
    if (task_create_kernel("dhcp_client", task_permissions_kernel, &id) != 0) {
        DEBUG_PRINT("[DHCP] failed to create client task\r\n");
        s->used = false;  // release the slot we reserved
        return;
    }
    s->task = id;
    if (task_start_kernel(id, (void *)dhcp_task, s) != 0) {
        DEBUG_PRINT("[DHCP] failed to start client task\r\n");
        s->used = false;  // no task will ever run this slot
        return;
    }
}
