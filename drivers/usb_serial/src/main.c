/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB-serial (FTDI) driver. Brings up an FTDI-based USB-serial adapter (QEMU's
 * `usb-serial`, VID 0x0403) and routes the GDB stub (SysGdb) over it, so the OS
 * can be debugged with GDB through a USB-serial dongle -- including on real
 * hardware that has no native serial port.
 *
 * FTDI: two bulk endpoints; every bulk-IN packet is prefixed with 2 status bytes
 * (modem/line status) which are stripped. To start a GDB session over the
 * adapter, trigger a breakpoint (gdb_stub_wait() / int3, or a GDB breakpoint);
 * the stub then talks RSP over this channel to a GDB connected to the dongle's
 * far end.
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

// Single instance is enough for the GDB channel use case.
static ftdi_dev_t the_ftdi;

// Pull one bulk-IN packet from the device; push its data bytes (after the 2
// FTDI status bytes) into the RX ring. Returns the number of data bytes added.
static int ftdi_fill(void) {
    ftdi_dev_t *f = &the_ftdi;
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

static int ftdi_getc(void) {
    ftdi_dev_t *f = &the_ftdi;
    while (f->rtail == f->rhead)
        ftdi_fill();  // block until a data byte arrives
    uint8_t c = f->ring[f->rtail];
    f->rtail = (f->rtail + 1) % (int)sizeof(f->ring);
    return c;
}

static void ftdi_putc(int c) {
    ftdi_dev_t *f = &the_ftdi;
    uint8_t b = (uint8_t)c;
    usb_dev_bulk(f->dev, f->out_ep, f->out_mps, &b, 1, 0);
}

static void ftdi_ctrl(usb_enum_device_t *dev, uint8_t req, uint16_t val, uint16_t idx) {
    usb_setup_packet_t s = {0x40, req, val, idx, 0};
    usb_dev_control(dev, &s, NULL, 0);
}

// Attach + async-break monitor. While no debugger is connected the system runs
// normally and this task polls the adapter for incoming GDB traffic. Any byte
// from GDB -- the initial RSP handshake on connect, or a lone 0x03 (Ctrl-C) sent
// to halt a running target -- drops us into the stub (int3) over this channel.
//
// This is USB-serial's stand-in for the COM2 UART RX interrupt: USB bulk-IN has
// no "byte arrived" IRQ, so we poll. After GDB resumes (continue/step/detach)
// the stub returns here and we keep polling, so a later Ctrl-C breaks in again;
// the stub reads g_resumed and re-sends the stop reply GDB expects.
//
// The received byte that triggered entry stays in the RX ring; the stub's
// recv_packet discards anything before '$', so a stray Ctrl-C is harmless. We
// only hold the USB lock inside the per-poll bulk transfer (released before
// gdb_stub_wait), and in-stub transfers run with interrupts off where xhci_wait
// self-pumps the event ring, so the session does not deadlock on the USB lock.
static void usb_serial_monitor(void *arg) {
    arg = NULL;
    while (true) {
        if (ftdi_fill() > 0) {
            DEBUG_PRINT("[usb_serial] GDB activity detected; entering debugger\r\n");
            gdb_stub_wait();  // int3 -> SysGdb stub, talking over this adapter
        }
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

    // Route the GDB stub over this adapter and start the attach monitor.
    gdb_set_channel(ftdi_getc, ftdi_putc);
    DEBUG_PRINT("[usb_serial] FTDI up; GDB available over USB-serial (connect GDB to attach)\r\n");
    cs_id t = 0;
    if (create_task_kernel("usb_serial_gdb_mon", task_permissions_kernel, &t) == CS_OK)
        start_task_kernel(t, usb_serial_monitor, NULL);
    return 0;
}

int module_init() {
    memset(&the_ftdi, 0, sizeof(the_ftdi));
    // FTDI is vendor-specific (interface class 0xFF); the probe filters by VID.
    usb_register_class_driver(0xFF, usb_serial_probe);
    DEBUG_PRINT("[usb_serial] registered FTDI USB-serial class driver\r\n");
    return 0;
}
