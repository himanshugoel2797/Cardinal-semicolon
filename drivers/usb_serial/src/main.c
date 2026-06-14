/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB-serial (FTDI) driver. Brings up an FTDI-based USB-serial adapter (QEMU's
 * `usb-serial`, VID 0x0403) and exposes it as a plain blocking byte channel.
 *
 * FTDI: two bulk endpoints; every bulk-IN packet is prefixed with 2 status bytes
 * (modem/line status) which are stripped.
 *
 * The adapter is offered to SysGdb via its transport-registration API
 * (gdb_register_transport): this driver provides only getc/putc/poll and carries
 * no GDB protocol knowledge -- SysGdb owns the RSP protocol and the async
 * break-in decision. (The link to SysGdb is a load-order dependency, not a
 * functional one; a future generic serial-registration server would remove even
 * that, letting the adapter come up with no debugger present.)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "CoreUsb/usb.h"
#include "SysGdb/gdb.h"
#include "SysTaskMgr/task.h"

#define FTDI_VID 0x0403

// FTDI vendor control requests.
#define FTDI_REQ_RESET 0x00
#define FTDI_REQ_SET_MODEM_CTRL 0x01
#define FTDI_REQ_SET_BAUDRATE 0x03

typedef struct {
    usb_enum_device_t *dev;
    int in_ep, out_ep;
    int in_mps, out_mps;
    bool in_use;
    // RX byte FIFO (data bytes only; FTDI status bytes already stripped).
    uint8_t ring[256];
    int rhead, rtail;
} ftdi_dev_t;

// Single instance is enough for the current single-adapter use case.
static ftdi_dev_t the_ftdi;

// Pull one bulk-IN packet from the device; push its data bytes (after the 2
// FTDI status bytes) into the RX ring. Returns the number of data bytes added.
static int ftdi_fill(ftdi_dev_t *f) {
    uint8_t buf[64];
    int n = f->in_mps > (int)sizeof(buf) ? (int)sizeof(buf) : f->in_mps;
    int r = usb_dev_bulk(f->dev, f->in_ep, f->in_mps, buf, n, 1);
    int added = 0;
    for (int i = 2; i < r; i++) {  // skip the 2 status bytes
        int next = (f->rhead + 1) % (int)sizeof(f->ring);
        if (next == f->rtail)
            break;  // ring full
        f->ring[f->rhead] = buf[i];
        f->rhead = next;
        added++;
    }
    return added;
}

// gdb_transport_t callbacks. `state` is the ftdi_dev_t*.
static int ftdi_getc(void *state) {
    ftdi_dev_t *f = (ftdi_dev_t *)state;
    while (f->rtail == f->rhead)
        ftdi_fill(f);  // block until a data byte arrives
    uint8_t c = f->ring[f->rtail];
    f->rtail = (f->rtail + 1) % (int)sizeof(f->ring);
    return c;
}

static void ftdi_putc(void *state, int c) {
    ftdi_dev_t *f = (ftdi_dev_t *)state;
    uint8_t b = (uint8_t)c;
    usb_dev_bulk(f->dev, f->out_ep, f->out_mps, &b, 1, 0);
}

// Non-blocking poll for async break-in: pump one bulk-IN packet, report whether
// any data bytes arrived. SysGdb decides what to do with that.
static int ftdi_poll(void *state) {
    return ftdi_fill((ftdi_dev_t *)state);
}

static void ftdi_ctrl(usb_enum_device_t *dev, uint8_t req, uint16_t val, uint16_t idx) {
    usb_setup_packet_t s = {0x40, req, val, idx, 0};
    usb_dev_control(dev, &s, NULL, 0);
}

// USB bulk-IN has no "byte arrived" IRQ, so a consumer that needs async break-in
// (SysGdb's Ctrl-C) has to poll. This task is that pump: it just hands control to
// gdb_poll_breakin(), which pumps our transport and owns the entire break-in
// decision. The driver itself stays GDB-agnostic. We only hold the USB lock
// inside the per-poll bulk transfer, and in-stub transfers run with interrupts
// off where xhci_wait self-pumps the event ring, so the session does not
// deadlock on the USB lock.
static void usb_serial_pump(void *arg) {
    arg = NULL;
    while (true) {
        gdb_poll_breakin();
        task_yield();
    }
}

static int usb_serial_probe(usb_enum_device_t *dev) {
    const usb_device_descriptor_t *d = usb_dev_descriptor(dev);
    if (d->idVendor != FTDI_VID) {
        DEBUG_PRINT("[usb_serial] not an FTDI device; not claiming\r\n");
        return -1;
    }
    int in_mps = 0, out_mps = 0;
    int in_ep = usb_dev_find_endpoint(dev, 2 /*bulk*/, 1 /*IN*/, &in_mps);
    int out_ep = usb_dev_find_endpoint(dev, 2 /*bulk*/, 0 /*OUT*/, &out_mps);
    if (in_ep < 0 || out_ep < 0) {
        DEBUG_PRINT("[usb_serial] missing bulk endpoints\r\n");
        return -1;
    }

    memset(&the_ftdi, 0, sizeof(the_ftdi));
    the_ftdi.dev = dev;
    the_ftdi.in_ep = in_ep;
    the_ftdi.out_ep = out_ep;
    the_ftdi.in_mps = in_mps > 0 ? in_mps : 64;
    the_ftdi.out_mps = out_mps > 0 ? out_mps : 64;
    the_ftdi.in_use = true;

    // Reset + assert DTR/RTS + set 115200 (divisor 0x001A for the FT232 base
    // clock; QEMU ignores the rate but real hardware needs it).
    ftdi_ctrl(dev, FTDI_REQ_RESET, 0, 1);
    ftdi_ctrl(dev, FTDI_REQ_SET_MODEM_CTRL, 0x0303, 1);  // DTR+RTS on
    ftdi_ctrl(dev, FTDI_REQ_SET_BAUDRATE, 0x001A, 0);

    // Offer this adapter to SysGdb as a byte transport and start the pump that
    // lets GDB's async Ctrl-C break in (USB bulk has no RX IRQ). SysGdb owns all
    // GDB protocol; we only provide byte I/O.
    gdb_transport_t transport = {
        .getc = ftdi_getc,
        .putc = ftdi_putc,
        .poll = ftdi_poll,
        .state = &the_ftdi,
    };
    gdb_register_transport(&transport);
    DEBUG_PRINT("[usb_serial] FTDI up; offered as GDB transport (connect GDB to attach)\r\n");
    cs_id t = 0;
    if (create_task_kernel("usb_serial_pump", task_permissions_kernel, &t) == CS_OK)
        start_task_kernel(t, usb_serial_pump, NULL);
    return 0;
}

int module_init() {
    memset(&the_ftdi, 0, sizeof(the_ftdi));
    // FTDI is vendor-specific (interface class 0xFF); the probe filters by VID.
    usb_register_class_driver(0xFF, usb_serial_probe);
    DEBUG_PRINT("[usb_serial] registered FTDI USB-serial class driver\r\n");
    return 0;
}
