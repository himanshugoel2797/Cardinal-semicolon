/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * xHCI (USB 3) host-controller driver. Implements the CoreUsb transfer backend
 * (control/interrupt/bulk) so the existing enumeration layer and class drivers
 * (HID, mass storage, hub) work over xHCI unchanged.
 *
 * xHCI manages addressing via commands (Enable Slot / Address Device) and slots,
 * which does not match CoreUsb's UHCI-style "software sends SET_ADDRESS" model.
 * The adapter: on port connect we Enable Slot + Address Device(BSR=1) so EP0 is
 * usable at the default address, then run CoreUsb enumeration; the control
 * handler intercepts SET_ADDRESS and issues Address Device(BSR=0) (xHCI assigns
 * the wire address; CoreUsb's address is just a handle we map to the slot).
 * Non-control endpoints are configured lazily on first transfer.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysTaskMgr/task.h"
#include "SysInterrupts/interrupts.h"
#include "pci/pci.h"
#include "CoreUsb/usb.h"

#include "xhci.h"

static xhci_ctrl_state_t *instances = NULL;
static int instance_lock = 0;

static int xhci_address_device(xhci_ctrl_state_t *s, xhci_slot_t *sl, int bsr);

// ---- MMIO accessors ----
static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline uint64_t rd64(volatile uint8_t *b, uint32_t off) { return *(volatile uint64_t *)(b + off); }
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) { *(volatile uint64_t *)(b + off) = v; }

// Allocate one zeroed, 32-bit, uncached DMA page; returns virt and writes phys.
static uint8_t *alloc_page(uintptr_t *phys_out) {
    uintptr_t p = pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data | physmem_alloc_flags_zero, KiB(4));
    if (p == PHYSMEM_NO_ALLOC)
        return NULL;
    *phys_out = p;
    return (uint8_t *)vmem_phystovirt((intptr_t)p, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
}

static void delay(uint64_t iters) {
    for (volatile uint64_t i = 0; i < iters; i++)
        ;
}

// ---- TRB ring ----
static void ring_init(xhci_ring_t *r) {
    uintptr_t phys;
    r->trbs = (xhci_trb_t *)alloc_page(&phys);
    r->phys = phys;
    r->enqueue = 0;
    r->cycle = 1;
    // Permanent Link TRB at the last slot, back to the start, Toggle Cycle.
    xhci_trb_t *link = &r->trbs[XHCI_RING_SIZE - 1];
    link->parameter = phys;
    link->status = 0;
    link->control = XHCI_TRB_SET_TYPE(TRB_LINK) | (1 << 1) /*TC*/;
}

// Push a TRB; returns its physical address. Handles the link/wrap.
static uintptr_t ring_push(xhci_ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *t = &r->trbs[r->enqueue];
    uintptr_t tphys = r->phys + (uintptr_t)r->enqueue * sizeof(xhci_trb_t);
    t->parameter = param;
    t->status = status;
    control = (control & ~XHCI_TRB_CYCLE) | (r->cycle ? XHCI_TRB_CYCLE : 0);
    t->control = control;

    r->enqueue++;
    if (r->enqueue == XHCI_RING_SIZE - 1) {
        // Reached the Link TRB: set its cycle to the producer cycle, wrap, toggle.
        xhci_trb_t *link = &r->trbs[XHCI_RING_SIZE - 1];
        link->control = XHCI_TRB_SET_TYPE(TRB_LINK) | (1 << 1) |
                        (r->cycle ? XHCI_TRB_CYCLE : 0);
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return tphys;
}

// ---- Event ring ----
// Pop the next event if one is available (cycle matches). Returns 1 + fills out.
static int next_event(xhci_ctrl_state_t *s, xhci_trb_t *out) {
    xhci_trb_t *e = &s->event_ring[s->event_idx];
    if (((e->control & XHCI_TRB_CYCLE) ? 1 : 0) != s->event_cycle)
        return 0;
    *out = *e;
    s->event_idx++;
    if (s->event_idx == XHCI_RING_SIZE) {
        s->event_idx = 0;
        s->event_cycle ^= 1;
    }
    // Advance ERDP (with EHB clear) to the new dequeue position.
    uintptr_t erdp = s->event_ring_phys + (uintptr_t)s->event_idx * sizeof(xhci_trb_t);
    wr64(s->rt, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp | (1 << 3) /*EHB clear write*/);
    return 1;
}

// Drain the event ring, recording the first command/transfer completion into
// irq_trb/irq_done. Port-status (and any other) events are consumed (ports are
// handled by the poll task reading PORTSC). Caller must hold event_lock with
// interrupts off (or be the ISR, which already runs IRQs-off).
static void xhci_drain_events(xhci_ctrl_state_t *s) {
    xhci_trb_t ev;
    while (next_event(s, &ev)) {
        int t = XHCI_TRB_TYPE(ev.control);
        if (t == TRB_EVENT_CMD_COMPLETE || t == TRB_EVENT_TRANSFER) {
            s->irq_trb = ev;
            s->irq_done = 1;
        }
    }
}

static inline int irqs_on(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return (int)((f >> 9) & 1);
}

// Wait for the outstanding operation's completion event. Interrupt-accelerated:
// the MSI ISR drains the ring and sets irq_done; we also drain it ourselves
// (under event_lock, IRQs off) so it still completes if an interrupt is missed
// or interrupts are disabled (e.g. from the GDB stub). Yields between polls when
// interrupts are enabled.
static int xhci_wait(xhci_ctrl_state_t *s, int type, int slot, xhci_trb_t *out, uint64_t spin_limit) {
    for (volatile uint64_t spin = 0; spin < spin_limit; spin++) {
        int cli_state = cli();
        local_spinlock_lock(&s->event_lock);
        xhci_drain_events(s);
        local_spinlock_unlock(&s->event_lock);
        sti(cli_state);

        if (s->irq_done) {
            xhci_trb_t ev = s->irq_trb;
            s->irq_done = 0;
            int etype = XHCI_TRB_TYPE(ev.control);
            int eslot = (ev.control >> 24) & 0xFF;
            if (etype == type && (slot < 0 || eslot == slot)) {
                if (out)
                    *out = ev;
                return XHCI_CC(ev.status);
            }
            // Mismatch shouldn't happen (one op outstanding under s->lock); drop.
        }
        if (irqs_on())
            task_yield();
    }
    return -1;
}

// MSI interrupt handler: ack the controller and drain the event ring for every
// xHCI instance (the vector is shared across the small instances list here).
static void xhci_isr(int vector) {
    vector = 0;
    for (xhci_ctrl_state_t *s = instances; s != NULL; s = s->next) {
        uint32_t sts = rd32(s->op, XHCI_OP_USBSTS);
        if (sts & XHCI_USBSTS_EINT)
            wr32(s->op, XHCI_OP_USBSTS, XHCI_USBSTS_EINT);  // W1C
        uint32_t iman = rd32(s->rt, XHCI_RT_IR0 + XHCI_IR_IMAN);
        if (iman & XHCI_IMAN_IP)
            wr32(s->rt, XHCI_RT_IR0 + XHCI_IR_IMAN, iman | XHCI_IMAN_IP);  // W1C IP, keep IE
        local_spinlock_lock(&s->event_lock);
        xhci_drain_events(s);
        local_spinlock_unlock(&s->event_lock);
    }
}

static int xhci_command(xhci_ctrl_state_t *s, uint64_t param, uint32_t control, xhci_trb_t *out) {
    ring_push(&s->cmd_ring, param, 0, control);
    s->irq_done = 0;
    s->db[0] = 0;  // ring command doorbell
    return xhci_wait(s, TRB_EVENT_CMD_COMPLETE, -1, out, 400000000ULL);
}

// ---- Context helpers ----
static uint32_t *slot_ctx(xhci_ctrl_state_t *s, uint8_t *input_ctx) {
    return (uint32_t *)(input_ctx + s->ctx_size);  // after the Input Control Context
}
static uint32_t *ep_ctx(xhci_ctrl_state_t *s, uint8_t *input_ctx, int dci) {
    return (uint32_t *)(input_ctx + s->ctx_size * (1 + dci));
}

static int speed_to_mps0(int xhci_speed) {
    if (xhci_speed == 4)
        return 512;  // super
    if (xhci_speed == 3)
        return 64;  // high
    return 8;       // low/full (safe minimum)
}

// ---- Endpoint transfer ring setup (Configure Endpoint) ----
static int configure_endpoint(xhci_ctrl_state_t *s, xhci_slot_t *sl, int dci, int ep_type, int mps) {
    if (sl->ep[dci].configured)
        return 0;

    ring_init(&sl->ep[dci].ring);

    uintptr_t in_phys;
    uint8_t *input = alloc_page(&in_phys);
    if (input == NULL)
        return -1;

    // Input Control Context: add slot (A0) + this endpoint (A<dci>).
    uint32_t *icc = (uint32_t *)input;
    icc[1] = (1u << 0) | (1u << dci);  // add flags

    // Slot context: bump Context Entries to cover this DCI.
    uint32_t *sc = slot_ctx(s, input);
    sc[0] = (sl->dev_ctx ? ((uint32_t *)sl->dev_ctx)[0] : 0);  // keep route/speed
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)dci << 27);  // context entries = dci
    sc[1] = (uint32_t)sl->root_port << 16;

    // Endpoint context.
    uint32_t *ec = ep_ctx(s, input, dci);
    ec[1] = ((uint32_t)ep_type << 3) | (3 << 1) | ((uint32_t)mps << 16);
    uint64_t trdp = sl->ep[dci].ring.phys | 1 /*DCS*/;
    ec[2] = (uint32_t)trdp;
    ec[3] = (uint32_t)(trdp >> 32);
    ec[4] = (mps & 0xFFFF);  // average TRB length hint

    xhci_trb_t out;
    int cc = xhci_command(s, in_phys, XHCI_TRB_SET_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)sl->slot_id << 24), &out);
    if (cc != XHCI_CC_SUCCESS)
        return -1;
    sl->ep[dci].configured = 1;
    return 0;
}

// ---- Transfers ----
static int do_control(xhci_ctrl_state_t *s, xhci_slot_t *sl, const usb_setup_packet_t *setup,
                      void *data, int data_len) {
    int is_read = (setup->bmRequestType & USB_REQ_DIR_IN) != 0;
    uint64_t setup_param;
    memcpy(&setup_param, setup, 8);

    int trt = (data_len == 0) ? 0 : (is_read ? 3 : 2);
    // Setup stage (immediate data).
    ring_push(&sl->ep[1].ring, setup_param, 8 /*TRB len*/,
              XHCI_TRB_SET_TYPE(TRB_SETUP) | (1 << 6) /*IDT*/ | ((uint32_t)trt << 16));

    // Data stage.
    if (data_len > 0) {
        if (data_len > XHCI_BOUNCE_MAX)
            data_len = XHCI_BOUNCE_MAX;
        if (!is_read && data != NULL)
            memcpy(s->bounce_virt, data, data_len);
        ring_push(&sl->ep[1].ring, s->bounce_phys, (uint32_t)data_len,
                  XHCI_TRB_SET_TYPE(TRB_DATA) | (is_read ? (1 << 16) : 0));
    }

    // Status stage (opposite direction, IOC).
    int status_dir = (is_read || data_len == 0) ? 0 : 1;  // IN data -> OUT status
    ring_push(&sl->ep[1].ring, 0, 0,
              XHCI_TRB_SET_TYPE(TRB_STATUS) | (status_dir ? (1 << 16) : 0) | (1 << 5) /*IOC*/);

    s->irq_done = 0;
    s->db[sl->slot_id] = 1;  // ring EP0 doorbell (DCI 1)

    xhci_trb_t out;
    int cc = xhci_wait(s, TRB_EVENT_TRANSFER, sl->slot_id, &out, 400000000ULL);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET)
        return -1;

    if (is_read && data != NULL && data_len > 0)
        memcpy(data, s->bounce_virt, data_len);
    return data_len;
}

static int do_data(xhci_ctrl_state_t *s, xhci_slot_t *sl, int dci, int ep_type, int mps,
                   void *data, int len, int dir_in, uint64_t timeout) {
    if (configure_endpoint(s, sl, dci, ep_type, mps) != 0)
        return -1;
    if (len > XHCI_BOUNCE_MAX)
        len = XHCI_BOUNCE_MAX;
    if (!dir_in && data != NULL && len > 0)
        memcpy(s->bounce_virt, data, len);

    ring_push(&sl->ep[dci].ring, s->bounce_phys, (uint32_t)len,
              XHCI_TRB_SET_TYPE(TRB_NORMAL) | (1 << 5) /*IOC*/ | (1 << 2) /*ISP*/);
    s->irq_done = 0;
    s->db[sl->slot_id] = dci;

    xhci_trb_t out;
    int cc = xhci_wait(s, TRB_EVENT_TRANSFER, sl->slot_id, &out, timeout);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        if (dir_in)
            return 0;  // no data this poll
        return -1;
    }
    int residual = out.status & 0xFFFFFF;
    int got = len - residual;
    if (got < 0)
        got = 0;
    if (dir_in && data != NULL && got > 0)
        memcpy(data, s->bounce_virt, got);
    return got;
}

// ---- CoreUsb handler implementations ----
static int xhci_control(void *hc_state, int dev_addr, usb_speed_t speed, int max_packet,
                        const usb_setup_packet_t *setup, void *data, int data_len) {
    (void)speed;
    (void)max_packet;
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);

    int slot_id = (dev_addr == 0) ? s->enumerating_slot : s->addr_to_slot[dev_addr & 0xFF];
    int ret = -1;
    if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS) {
        xhci_slot_t *sl = &s->slots[slot_id];
        // Intercept SET_ADDRESS: xHCI assigns the wire address via Address Device
        // (BSR=0). CoreUsb's requested address is just a handle we map to the slot.
        if (setup->bmRequestType == 0x00 && setup->bRequest == USB_REQ_SET_ADDRESS) {
            ret = (xhci_address_device(s, sl, 0) == 0) ? 0 : -1;
            if (ret == 0)
                s->addr_to_slot[setup->wValue & 0xFF] = slot_id;
        } else {
            ret = do_control(s, sl, setup, data, data_len);
        }
    }

    local_spinlock_unlock(&s->lock);
    return ret;
}

static int xhci_interrupt_in(void *hc_state, int dev_addr, usb_speed_t speed, int endpoint,
                             int max_packet, int data_toggle, void *data, int len) {
    (void)speed;
    (void)data_toggle;
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);
    int slot_id = s->addr_to_slot[dev_addr & 0xFF];
    int ret = -1;
    if (slot_id > 0) {
        int ep = endpoint & 0xF;
        int dci = ep * 2 + 1;  // IN
        ret = do_data(s, &s->slots[slot_id], dci, EP_TYPE_INTR_IN, max_packet, data, len, 1, 8000000ULL);
    }
    local_spinlock_unlock(&s->lock);
    return ret;
}

static int xhci_bulk(void *hc_state, int dev_addr, int endpoint, int max_packet,
                     int data_toggle, void *data, int len, int dir_in) {
    (void)data_toggle;
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);
    int slot_id = s->addr_to_slot[dev_addr & 0xFF];
    int ret = -1;
    if (slot_id > 0) {
        int ep = endpoint & 0xF;
        int dci = ep * 2 + (dir_in ? 1 : 0);
        int type = dir_in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
        ret = do_data(s, &s->slots[slot_id], dci, type, max_packet, data, len, dir_in, 300000000ULL);
    }
    local_spinlock_unlock(&s->lock);
    return ret;
}

// Address Device command; bsr=1 sets up EP0 without sending SET_ADDRESS.
static int xhci_address_device(xhci_ctrl_state_t *s, xhci_slot_t *sl, int bsr) {
    uintptr_t in_phys;
    uint8_t *input = alloc_page(&in_phys);
    if (input == NULL)
        return -1;
    uint32_t *icc = (uint32_t *)input;
    icc[1] = (1u << 0) | (1u << 1);  // add slot + ep0

    uint32_t *sc = slot_ctx(s, input);
    // dword0: Route String[19:0], Speed[23:20], Context Entries[31:27].
    sc[0] = (sl->route & 0xFFFFF) | ((uint32_t)sl->speed << 20) | ((uint32_t)1 << 27);
    // dword1: Root Hub Port Number[23:16].
    sc[1] = (uint32_t)sl->root_port << 16;

    uint32_t *ec = ep_ctx(s, input, 1);
    int mps0 = speed_to_mps0(sl->speed);
    ec[1] = ((uint32_t)EP_TYPE_CONTROL << 3) | (3 << 1) | ((uint32_t)mps0 << 16);
    uint64_t trdp = sl->ep[1].ring.phys | 1;
    ec[2] = (uint32_t)trdp;
    ec[3] = (uint32_t)(trdp >> 32);
    ec[4] = 8;

    xhci_trb_t out;
    int cc = xhci_command(s, in_phys,
                          XHCI_TRB_SET_TYPE(TRB_ADDRESS_DEVICE) | (bsr ? (1 << 9) : 0) | ((uint32_t)sl->slot_id << 24),
                          &out);
    return (cc == XHCI_CC_SUCCESS) ? 0 : -1;
}

// Map a CoreUsb usb_speed_t to the xHCI speed id (1=full,2=low,3=high,4=super).
static int uspeed_to_xspeed(usb_speed_t s) {
    return (s == usb_speed_low) ? 2 : (s == usb_speed_high) ? 3 : (s == usb_speed_super) ? 4 : 1;
}

// Prepare a slot for a device on a hub's downstream port: Enable Slot + Address
// Device(BSR=1) with the route string derived from the parent hub's slot, so the
// following default-address control transfers reach the new device.
static int xhci_prepare_downstream(void *hc_state, int parent_addr, int parent_port, usb_speed_t speed) {
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);
    int ret = -1;

    int parent_slot = s->addr_to_slot[parent_addr & 0xFF];
    if (parent_slot > 0 && parent_slot <= XHCI_MAX_SLOTS && s->slots[parent_slot].in_use) {
        xhci_slot_t *psl = &s->slots[parent_slot];
        xhci_trb_t out;
        int cc = xhci_command(s, 0, XHCI_TRB_SET_TYPE(TRB_ENABLE_SLOT), &out);
        if (cc == XHCI_CC_SUCCESS) {
            int slot_id = (out.control >> 24) & 0xFF;
            if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS) {
                xhci_slot_t *sl = &s->slots[slot_id];
                memset(sl, 0, sizeof(*sl));
                sl->slot_id = slot_id;
                sl->speed = uspeed_to_xspeed(speed);
                sl->root_port = psl->root_port;
                sl->route = psl->route | ((uint32_t)(parent_port & 0xF) << (4 * psl->depth));
                sl->depth = psl->depth + 1;
                sl->in_use = 1;

                sl->dev_ctx = alloc_page(&sl->dev_ctx_phys);
                s->dcbaa[slot_id] = sl->dev_ctx_phys;
                ring_init(&sl->ep[1].ring);
                sl->ep[1].configured = 1;

                if (xhci_address_device(s, sl, 1) == 0) {
                    s->enumerating_slot = slot_id;
                    ret = 0;
                }
            }
        }
    }
    local_spinlock_unlock(&s->lock);
    return ret;
}

// Mark a device's slot as a hub (Hub bit + Number of Ports) via Configure
// Endpoint, so the controller routes packets to devices behind it.
static int xhci_mark_hub(void *hc_state, int dev_addr, int nports) {
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);
    int ret = -1;
    int slot_id = s->addr_to_slot[dev_addr & 0xFF];
    if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS && s->slots[slot_id].in_use) {
        xhci_slot_t *sl = &s->slots[slot_id];
        uintptr_t in_phys;
        uint8_t *input = alloc_page(&in_phys);
        if (input != NULL) {
            uint32_t *icc = (uint32_t *)input;
            icc[1] = (1u << 0);  // add slot context (A0) only
            uint32_t *sc = slot_ctx(s, input);
            uint32_t *dsc = (uint32_t *)sl->dev_ctx;  // current slot context
            sc[0] = dsc[0] | (1u << 26);  // keep route/speed/ctx-entries, set Hub
            sc[1] = (dsc[1] & 0x00FFFFFF) | ((uint32_t)(nports & 0xFF) << 24);  // Number of Ports
            sc[2] = dsc[2];
            sc[3] = dsc[3];
            xhci_trb_t out;
            int cc = xhci_command(s, in_phys,
                                  XHCI_TRB_SET_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24), &out);
            ret = (cc == XHCI_CC_SUCCESS) ? 0 : -1;
        }
    }
    local_spinlock_unlock(&s->lock);
    return ret;
}

// ---- Port handling / enumeration ----
static void xhci_port_connected(xhci_ctrl_state_t *s, int port) {
    // Reset the port.
    uint32_t psc = rd32(s->op, XHCI_OP_PORTSC(port));
    wr32(s->op, XHCI_OP_PORTSC(port), (psc & ~(XHCI_PORTSC_PED)) | XHCI_PORTSC_PR);
    for (volatile uint64_t i = 0; i < 50000000; i++)
        if (rd32(s->op, XHCI_OP_PORTSC(port)) & XHCI_PORTSC_PRC)
            break;
    // ack change bits
    psc = rd32(s->op, XHCI_OP_PORTSC(port));
    wr32(s->op, XHCI_OP_PORTSC(port), psc | XHCI_PORTSC_PRC | XHCI_PORTSC_CSC);
    if (!(rd32(s->op, XHCI_OP_PORTSC(port)) & XHCI_PORTSC_PED)) {
        DEBUG_PRINT("[xHCI] port not enabled after reset\r\n");
        return;
    }
    int xspeed = (rd32(s->op, XHCI_OP_PORTSC(port)) >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;

    // Submit Enable Slot + Address Device(BSR=1) under s->lock so they don't race
    // with a concurrent transfer for an already-enumerated device on the irq_done
    // single-outstanding-op flag. Released before usb_port_connected, which
    // re-enters xhci_control (which also takes s->lock) during enumeration.
    local_spinlock_lock(&s->lock);

    // Enable a slot.
    xhci_trb_t out;
    int cc = xhci_command(s, 0, XHCI_TRB_SET_TYPE(TRB_ENABLE_SLOT), &out);
    if (cc != XHCI_CC_SUCCESS) {
        local_spinlock_unlock(&s->lock);
        DEBUG_PRINT("[xHCI] Enable Slot failed\r\n");
        return;
    }
    int slot_id = (out.control >> 24) & 0xFF;
    if (slot_id <= 0 || slot_id > XHCI_MAX_SLOTS) {
        local_spinlock_unlock(&s->lock);
        return;
    }

    xhci_slot_t *sl = &s->slots[slot_id];
    memset(sl, 0, sizeof(*sl));
    sl->slot_id = slot_id;
    sl->speed = xspeed;
    sl->root_port = port;
    sl->in_use = 1;

    // Device context + DCBAA entry; EP0 ring.
    sl->dev_ctx = alloc_page(&sl->dev_ctx_phys);
    s->dcbaa[slot_id] = sl->dev_ctx_phys;
    ring_init(&sl->ep[1].ring);
    sl->ep[1].configured = 1;

    // Address Device with BSR=1: EP0 usable, device still at address 0.
    if (xhci_address_device(s, sl, 1) != 0) {
        local_spinlock_unlock(&s->lock);
        DEBUG_PRINT("[xHCI] Address Device (BSR=1) failed\r\n");
        return;
    }

    s->enumerating_slot = slot_id;
    local_spinlock_unlock(&s->lock);

    usb_speed_t uspeed = (xspeed == 2) ? usb_speed_low
                         : (xspeed == 3) ? usb_speed_high
                         : (xspeed == 4) ? usb_speed_super
                                         : usb_speed_full;
    DEBUG_PRINT("[xHCI] port up; enumerating\r\n");
    usb_port_connected(s->handle, port, uspeed);
}

// Free a slot's DMA: the per-endpoint transfer rings and the device context.
static void xhci_free_slot_mem(xhci_slot_t *sl) {
    for (int dci = 1; dci < 31; dci++) {
        if (sl->ep[dci].configured && sl->ep[dci].ring.trbs != NULL) {
            pagealloc_free(sl->ep[dci].ring.phys, KiB(4));
            sl->ep[dci].ring.trbs = NULL;
            sl->ep[dci].configured = 0;
        }
    }
    if (sl->dev_ctx != NULL) {
        pagealloc_free(sl->dev_ctx_phys, KiB(4));
        sl->dev_ctx = NULL;
    }
}

// CoreUsb disconnect handler: Disable Slot for the device at `dev_addr`, drop its
// DCBAA entry, free its contexts/rings, and release the slot + address mapping.
static void xhci_disconnect(void *hc_state, int dev_addr) {
    xhci_ctrl_state_t *s = (xhci_ctrl_state_t *)hc_state;
    local_spinlock_lock(&s->lock);
    int slot_id = s->addr_to_slot[dev_addr & 0xFF];
    if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS && s->slots[slot_id].in_use) {
        xhci_trb_t out;
        xhci_command(s, 0, XHCI_TRB_SET_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), &out);
        s->dcbaa[slot_id] = 0;
        xhci_free_slot_mem(&s->slots[slot_id]);
        memset(&s->slots[slot_id], 0, sizeof(s->slots[slot_id]));
        DEBUG_PRINT("[xHCI] slot disabled for disconnected device\r\n");
    }
    s->addr_to_slot[dev_addr & 0xFF] = 0;
    local_spinlock_unlock(&s->lock);
}

static void xhci_poll_task(xhci_ctrl_state_t *s) {
    while (!s->init_complete)
        ;
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    while (true) {
        for (int p = 1; p <= s->max_ports; p++) {
            uint32_t psc = rd32(s->op, XHCI_OP_PORTSC(p));
            bool conn = (psc & XHCI_PORTSC_CCS) != 0;
            if (conn && !seen[p]) {
                seen[p] = true;
                xhci_port_connected(s, p);
            } else if (!conn && seen[p]) {
                seen[p] = false;
                DEBUG_PRINT("[xHCI] port down; disconnecting\r\n");
                usb_port_disconnected(s->handle, p);
            }
        }
        task_yield();
    }
}

int module_init(void *ecam_addr) {
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    device->command.busmaster = 1;
    device->command.mem_space = 1;

    xhci_ctrl_state_t *s = malloc(sizeof(xhci_ctrl_state_t));
    memset(s, 0, sizeof(*s));

    int cli_state = cli();
    local_spinlock_lock(&instance_lock);
    s->next = instances;
    instances = s;
    local_spinlock_unlock(&instance_lock);
    sti(cli_state);

    uint64_t bar = ((uint64_t)(device->bar[0] & ~0xFu)) | ((uint64_t)device->bar[1] << 32);
    s->mmio = (volatile uint8_t *)vmem_phystovirt((intptr_t)bar, KiB(64), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    uint8_t caplength = *(volatile uint8_t *)(s->mmio + XHCI_CAP_CAPLENGTH);
    s->op = s->mmio + caplength;
    s->rt = s->mmio + (rd32(s->mmio, XHCI_CAP_RTSOFF) & ~0x1Fu);
    s->db = (volatile uint32_t *)(s->mmio + (rd32(s->mmio, XHCI_CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = rd32(s->mmio, XHCI_CAP_HCSPARAMS1);
    s->max_slots = hcs1 & 0xFF;
    s->max_ports = (hcs1 >> 24) & 0xFF;
    uint32_t hcc1 = rd32(s->mmio, XHCI_CAP_HCCPARAMS1);
    s->ctx_size = (hcc1 & (1 << 2)) ? 64 : 32;

    // Reset the controller.
    wr32(s->op, XHCI_OP_USBCMD, 0);
    for (volatile uint64_t i = 0; i < 50000000; i++)
        if (rd32(s->op, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)
            break;
    wr32(s->op, XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);
    for (volatile uint64_t i = 0; i < 100000000; i++)
        if (!(rd32(s->op, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) && !(rd32(s->op, XHCI_OP_USBSTS) & XHCI_USBSTS_CNR))
            break;

    int slots_en = s->max_slots > XHCI_MAX_SLOTS ? XHCI_MAX_SLOTS : s->max_slots;
    wr32(s->op, XHCI_OP_CONFIG, slots_en);

    // DCBAA.
    s->dcbaa = (uint64_t *)alloc_page(&s->dcbaa_phys);
    wr64(s->op, XHCI_OP_DCBAAP, s->dcbaa_phys);

    // Command ring.
    ring_init(&s->cmd_ring);
    wr64(s->op, XHCI_OP_CRCR, s->cmd_ring.phys | 1 /*RCS*/);

    // Event ring + ERST (single segment).
    uintptr_t er_phys;
    s->event_ring = (xhci_trb_t *)alloc_page(&er_phys);
    s->event_ring_phys = er_phys;
    s->event_cycle = 1;
    s->event_idx = 0;
    s->erst = alloc_page(&s->erst_phys);
    *(uint64_t *)(s->erst + 0) = er_phys;          // ring segment base
    *(uint32_t *)(s->erst + 8) = XHCI_RING_SIZE;   // segment size
    wr32(s->rt, XHCI_RT_IR0 + XHCI_IR_ERSTSZ, 1);
    wr64(s->rt, XHCI_RT_IR0 + XHCI_IR_ERDP, er_phys);
    wr64(s->rt, XHCI_RT_IR0 + XHCI_IR_ERSTBA, s->erst_phys);

    // Bounce buffer for transfers.
    s->bounce_virt = alloc_page(&s->bounce_phys);

    // MSI: allocate a vector, register the ISR, program the device's MSI cap.
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);
    int msi_vector = 0;
    interrupt_allocate(1, interrupt_flags_exclusive, &msi_vector);
    interrupt_registerhandler(msi_vector, xhci_isr);
    s->irq_vector = msi_vector;
    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = (uint32_t)msi_register_data(msi_vector);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    // Enable interrupter 0 (IMAN.IE); moderate to avoid an event-interrupt storm.
    wr32(s->rt, XHCI_RT_IR0 + XHCI_IR_IMOD, 4000);  // ~1ms interval
    wr32(s->rt, XHCI_RT_IR0 + XHCI_IR_IMAN, XHCI_IMAN_IE);

    // Run, with controller-level interrupt enable (USBCMD.INTE).
    wr32(s->op, XHCI_OP_USBCMD, XHCI_USBCMD_RS | XHCI_USBCMD_INTE);

    // Register with CoreUsb.
    usb_hci_desc_t *desc = malloc(sizeof(usb_hci_desc_t));
    memset(desc, 0, sizeof(*desc));
    strncpy(desc->name, "xhci", 16);
    desc->state = s;
    desc->device_type = usb_device_type_xhci;
    desc->handlers.control = xhci_control;
    desc->handlers.interrupt_in = xhci_interrupt_in;
    desc->handlers.bulk = xhci_bulk;
    desc->handlers.prepare_downstream = xhci_prepare_downstream;
    desc->handlers.mark_hub = xhci_mark_hub;
    desc->handlers.disconnect = xhci_disconnect;
    usb_register_hostcontroller(desc, &s->handle);

    cs_id task = 0;
    create_task_kernel("xhci_poll", task_permissions_kernel, &task);
    start_task_kernel(task, (void (*)(void *))xhci_poll_task, s);

    // Power all ports.
    for (int p = 1; p <= s->max_ports; p++) {
        uint32_t psc = rd32(s->op, XHCI_OP_PORTSC(p));
        wr32(s->op, XHCI_OP_PORTSC(p), psc | XHCI_PORTSC_PP);
    }
    delay(20000000);

    s->init_complete = true;
    DEBUG_PRINT("[xHCI] init complete\r\n");
    return 0;
}
