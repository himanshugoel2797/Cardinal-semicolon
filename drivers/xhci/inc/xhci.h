// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_XHCI_H
#define CARDINAL_SEMI_XHCI_H

#include <stdint.h>

// ---- Capability registers (at MMIO base) ----
#define XHCI_CAP_CAPLENGTH 0x00  // u8
#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF 0x14
#define XHCI_CAP_RTSOFF 0x18

// ---- Operational registers (at base + CAPLENGTH) ----
#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_DNCTRL 0x14
#define XHCI_OP_CRCR 0x18    // 64-bit
#define XHCI_OP_DCBAAP 0x30  // 64-bit
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC(p) (0x400 + ((p) - 1) * 0x10)  // p is 1-based

#define XHCI_USBCMD_RS (1 << 0)
#define XHCI_USBCMD_HCRST (1 << 1)
#define XHCI_USBCMD_INTE (1 << 2)
#define XHCI_USBSTS_HCH (1 << 0)
#define XHCI_USBSTS_EINT (1 << 3)  // event interrupt (write 1 to clear)
#define XHCI_USBSTS_CNR (1 << 11)  // controller not ready

#define XHCI_IMAN_IP (1 << 0)  // interrupt pending (write 1 to clear)
#define XHCI_IMAN_IE (1 << 1)  // interrupt enable

#define XHCI_PORTSC_CCS (1 << 0)   // current connect status
#define XHCI_PORTSC_PED (1 << 1)   // port enabled
#define XHCI_PORTSC_PR (1 << 4)    // port reset
#define XHCI_PORTSC_PP (1 << 9)    // port power
#define XHCI_PORTSC_CSC (1 << 17)  // connect status change
#define XHCI_PORTSC_PRC (1 << 21)  // port reset change
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK 0xF

// ---- Runtime registers (at base + RTSOFF); interrupter 0 at +0x20 ----
#define XHCI_RT_IR0 0x20
#define XHCI_IR_IMAN 0x00
#define XHCI_IR_IMOD 0x04
#define XHCI_IR_ERSTSZ 0x08
#define XHCI_IR_ERSTBA 0x10  // 64-bit
#define XHCI_IR_ERDP 0x18    // 64-bit

// ---- TRB ----
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} PACKED xhci_trb_t;

#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE(c) (((c) >> 10) & 0x3F)
#define XHCI_TRB_SET_TYPE(t) (((t) & 0x3F) << 10)
#define XHCI_TRB_CYCLE (1 << 0)

// TRB types
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVENT_TRANSFER 32
#define TRB_EVENT_CMD_COMPLETE 33
#define TRB_EVENT_PORT_STATUS 34

// Completion codes (event status bits 31:24)
#define XHCI_CC_SUCCESS 1
#define XHCI_CC_SHORT_PACKET 13
#define XHCI_CC(status) (((status) >> 24) & 0xFF)

// Endpoint context EP types
#define EP_TYPE_CONTROL 4
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_BULK_IN 6
#define EP_TYPE_INTR_OUT 3
#define EP_TYPE_INTR_IN 7

#define XHCI_MAX_SLOTS 64
#define XHCI_RING_SIZE 64  // TRBs per ring (must hold a transfer + link)
#define XHCI_BOUNCE_MAX 2048  // max single transfer data size (bounce buffer)

// A simple TRB ring (producer side) with a software cycle bit.
typedef struct {
    xhci_trb_t *trbs;    // virt
    uintptr_t phys;
    int enqueue;
    int cycle;  // producer cycle state
} xhci_ring_t;

// Per-endpoint software state.
typedef struct {
    xhci_ring_t ring;
    int configured;
} xhci_ep_t;

// Per-slot (device) software state.
typedef struct {
    int slot_id;
    uint8_t *dev_ctx;  // virt
    uintptr_t dev_ctx_phys;
    int speed;
    int root_port;
    uint32_t route;  // xHCI route string (0 for a device on a root port)
    int depth;       // hub tiers above this device (0 on a root port)
    xhci_ep_t ep[32];  // indexed by DCI 1..31 (slot 0 unused); size 32 so DCI 31 (EP 15 IN) is in-bounds
    int in_use;
} xhci_slot_t;

typedef struct xhci_ctrl_state {
    volatile uint8_t *mmio;  // base (capability) registers
    volatile uint8_t *op;    // operational registers
    volatile uint8_t *rt;    // runtime registers
    volatile uint32_t *db;   // doorbell array
    int ctx_size;            // 32 or 64 (from HCCPARAMS CSZ)
    int max_ports;
    int max_slots;

    // DCBAA
    uint64_t *dcbaa;  // virt
    uintptr_t dcbaa_phys;

    // Command ring
    xhci_ring_t cmd_ring;

    // Event ring + ERST
    xhci_trb_t *event_ring;  // virt
    uintptr_t event_ring_phys;
    int event_cycle;
    int event_idx;
    uint8_t *erst;  // virt
    uintptr_t erst_phys;

    xhci_slot_t slots[XHCI_MAX_SLOTS + 1];  // indexed by slot id (1-based)

    // DMA bounce buffer for transfer data stages.
    uint8_t *bounce_virt;
    uintptr_t bounce_phys;

    void *handle;       // CoreUsb HC handle
    int lock;           // serialises command/transfer submission
    int enumerating_slot;  // slot currently at USB address 0 (during enumeration)
    int addr_to_slot[256]; // USB address -> slot id

    // Interrupt-driven completion: the ISR (or the polled fallback) drains the
    // event ring and records the command/transfer completion here.
    int event_lock;            // guards event-ring draining (held with IRQs off)
    volatile int irq_done;     // set when a command/transfer event is recorded
    xhci_trb_t irq_trb;        // the recorded completion event
    int irq_vector;

    volatile bool init_complete;
    struct xhci_ctrl_state *next;
} xhci_ctrl_state_t;

#endif
