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
 * break-in decision.
 *
 * The dependency on SysGdb is OPTIONAL: the driver resolves the two SysGdb hooks
 * by name at runtime (symboldb_findfunc) rather than linking them, so it has no
 * undefined SysGdb symbols and loads fine in a build with no debugger. If SysGdb
 * is absent the adapter simply comes up idle (no consumer yet); a future generic
 * serial-registration server would give it one.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "CoreUsb/usb.h"
#include "SysGdb/gdb.h"  // gdb_transport_t (type only -- no symbol is imported)
#include "SysTaskMgr/task.h"
#include "symbol_db.h"   // symboldb_findfunc: resolve SysGdb hooks optionally

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
    cs_id task;
    volatile bool stop;     // remove() requests the pump task to exit
    volatile bool stopped;  // pump task acknowledges it has exited
    // RX byte FIFO (data bytes only; FTDI status bytes already stripped).
    uint8_t ring[256];
    int rhead, rtail;
} ftdi_dev_t;

// Single instance is enough for the current single-adapter use case.
static ftdi_dev_t the_ftdi;

// SysGdb hooks, resolved by name at runtime so the link is optional (NULL when
// SysGdb is not loaded). gdb_transport_t is just a type from the header.
static void (*gdb_register_transport_fn)(const gdb_transport_t *) = NULL;
static void (*gdb_unregister_transport_fn)(const gdb_transport_t *) = NULL;
static int (*gdb_poll_breakin_fn)(void) = NULL;

static void resolve_gdb_hooks(void) {
    Elf64_Shdr *h;
    Elf64_Sym *s;
    if (symboldb_findfunc("gdb_register_transport", &h, &s) == 0)
        gdb_register_transport_fn = (void (*)(const gdb_transport_t *))s->st_value;
    if (symboldb_findfunc("gdb_unregister_transport", &h, &s) == 0)
        gdb_unregister_transport_fn = (void (*)(const gdb_transport_t *))s->st_value;
    if (symboldb_findfunc("gdb_poll_breakin", &h, &s) == 0)
        gdb_poll_breakin_fn = (int (*)(void))s->st_value;
}

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
    ftdi_dev_t *f = (ftdi_dev_t *)arg;
    while (!f->stop) {
        if (gdb_poll_breakin_fn != NULL)
            gdb_poll_breakin_fn();
        task_yield();
    }
    f->stopped = true;  // hand off: remove() may now reclaim the device
    task_end_kernel(task_current());
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
    the_ftdi.stop = false;
    the_ftdi.stopped = false;
    the_ftdi.task = 0;
    the_ftdi.in_use = true;

    // Reset + assert DTR/RTS + set 115200 (divisor 0x001A for the FT232 base
    // clock; QEMU ignores the rate but real hardware needs it).
    ftdi_ctrl(dev, FTDI_REQ_RESET, 0, 1);
    ftdi_ctrl(dev, FTDI_REQ_SET_MODEM_CTRL, 0x0303, 1);  // DTR+RTS on
    ftdi_ctrl(dev, FTDI_REQ_SET_BAUDRATE, 0x001A, 0);

    // If SysGdb is present, offer this adapter to it as a byte transport and
    // start the pump that lets GDB's async Ctrl-C break in (USB bulk has no RX
    // IRQ). SysGdb owns all GDB protocol; we only provide byte I/O. If SysGdb is
    // absent the adapter just comes up idle.
    resolve_gdb_hooks();
    if (gdb_register_transport_fn != NULL) {
        gdb_transport_t transport = {
            .getc = ftdi_getc,
            .putc = ftdi_putc,
            .poll = ftdi_poll,
            .state = &the_ftdi,
        };
        gdb_register_transport_fn(&transport);
        DEBUG_PRINT("[usb_serial] FTDI up; offered as GDB transport (connect GDB to attach)\r\n");
        if (task_create_kernel("usb_serial_pump", task_permissions_kernel, &the_ftdi.task) == CS_OK)
            task_start_kernel(the_ftdi.task, usb_serial_pump, &the_ftdi);
    } else {
        DEBUG_PRINT("[usb_serial] FTDI up; SysGdb not loaded, adapter idle\r\n");
    }
    return 0;
}

static void usb_serial_remove(usb_enum_device_t *dev) {
    ftdi_dev_t *f = &the_ftdi;
    if (!f->in_use || f->dev != dev)
        return;
    // Stop the pump (if one was started) and wait for it to exit.
    if (f->task != 0) {
        f->stop = true;
        while (!f->stopped)
            task_yield();
    }
    // Revert the GDB channel to COM2 so the stub never talks over a dead adapter.
    if (gdb_unregister_transport_fn != NULL) {
        gdb_transport_t transport = {
            .getc = ftdi_getc, .putc = ftdi_putc, .poll = ftdi_poll, .state = f,
        };
        gdb_unregister_transport_fn(&transport);
    }
    f->in_use = false;
    DEBUG_PRINT("[usb_serial] device removed\r\n");
}

int module_init() {
    memset(&the_ftdi, 0, sizeof(the_ftdi));
    // FTDI is vendor-specific (interface class 0xFF); the probe filters by VID.
    usb_register_class_driver(0xFF, usb_serial_probe, usb_serial_remove);
    DEBUG_PRINT("[usb_serial] registered FTDI USB-serial class driver\r\n");
    return 0;
}
