/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB HID boot-protocol driver (keyboard + mouse). Registers as a CoreUsb class
 * driver for bInterfaceClass==HID; on probe it selects the boot protocol and
 * spawns a task that polls the interrupt IN endpoint and decodes reports.
 *
 * Test interface (intentionally simple, to be reviewed/redesigned): decoded
 * input is currently printed to the debug console rather than delivered to
 * CoreInput. CoreInput uses a pull/read-callback model that wants an event
 * queue; wiring that up is the next step (see notes/servers/CoreUsb-roadmap.md).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "CoreUsb/usb.h"
#include "SysTaskMgr/task.h"

#define HID_PROTO_KEYBOARD 1
#define HID_PROTO_MOUSE 2

// HID class requests.
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

typedef struct {
    usb_enum_device_t *dev;
    int endpoint;
    int max_packet;
    int proto;
    int iface;
    bool in_use;
    uint8_t last[8];
} hid_dev_t;

#define MAX_HID 4
static hid_dev_t hid_devs[MAX_HID];

static char hid_keycode_to_char(uint8_t k) {
    if (k >= 0x04 && k <= 0x1D)
        return (char)('a' + (k - 0x04));
    if (k >= 0x1E && k <= 0x26)
        return (char)('1' + (k - 0x1E));
    if (k == 0x27)
        return '0';
    if (k == 0x2C)
        return ' ';
    if (k == 0x28)
        return '\n';
    return 0;
}

static void hid_decode_keyboard(hid_dev_t *h, uint8_t *rpt, int len) {
    if (len < 8)
        return;
    // rpt[0]=modifiers, rpt[2..7]=keycodes. Print keys newly pressed since the
    // last report (simple debounce against the previous report).
    for (int i = 2; i < 8; i++) {
        uint8_t k = rpt[i];
        if (k == 0)
            continue;
        bool was_down = false;
        for (int j = 2; j < 8; j++)
            if (h->last[j] == k)
                was_down = true;
        if (!was_down) {
            char c = hid_keycode_to_char(k);
            char buf[8];
            DEBUG_PRINT("[usb_hid] key down: usage=0x");
            DEBUG_PRINT(itoa(k, buf, 16));
            if (c) {
                DEBUG_PRINT(" ('");
                char s[2] = {c == '\n' ? '?' : c, 0};
                DEBUG_PRINT(s);
                DEBUG_PRINT("')");
            }
            DEBUG_PRINT("\r\n");
        }
    }
    memcpy(h->last, rpt, 8);
}

static void hid_decode_mouse(uint8_t *rpt, int len) {
    if (len < 3)
        return;
    int8_t dx = (int8_t)rpt[1];
    int8_t dy = (int8_t)rpt[2];
    char buf[12];
    DEBUG_PRINT("[usb_hid] mouse buttons=0x");
    DEBUG_PRINT(itoa(rpt[0], buf, 16));
    DEBUG_PRINT(" dx=");
    DEBUG_PRINT(itoa(dx, buf, 10));
    DEBUG_PRINT(" dy=");
    DEBUG_PRINT(itoa(dy, buf, 10));
    DEBUG_PRINT("\r\n");
}

static void hid_poll_task(void *arg) {
    hid_dev_t *h = (hid_dev_t *)arg;
    DEBUG_PRINT("[usb_hid] polling started\r\n");
    while (true) {
        uint8_t rpt[8];
        memset(rpt, 0, sizeof(rpt));
        int n = h->max_packet;
        if (n > (int)sizeof(rpt))
            n = sizeof(rpt);
        int r = usb_dev_interrupt_in(h->dev, h->endpoint, n, rpt, n);
        if (r > 0) {
            if (h->proto == HID_PROTO_KEYBOARD)
                hid_decode_keyboard(h, rpt, r);
            else if (h->proto == HID_PROTO_MOUSE)
                hid_decode_mouse(rpt, r);
            else {
                char buf[8];
                DEBUG_PRINT("[usb_hid] report len=");
                DEBUG_PRINT(itoa(r, buf, 10));
                DEBUG_PRINT("\r\n");
            }
        }
        task_yield();
    }
}

static int hid_probe(usb_enum_device_t *dev) {
    int mps = 0;
    int ep = usb_dev_find_endpoint(dev, 3 /*interrupt*/, 1 /*IN*/, &mps);
    if (ep < 0) {
        DEBUG_PRINT("[usb_hid] no interrupt IN endpoint; not claiming\r\n");
        return -1;
    }

    hid_dev_t *h = NULL;
    for (int i = 0; i < MAX_HID; i++)
        if (!hid_devs[i].in_use) {
            h = &hid_devs[i];
            break;
        }
    if (h == NULL)
        return -1;

    h->dev = dev;
    h->endpoint = ep;
    h->max_packet = mps > 0 ? mps : 8;
    h->proto = usb_dev_interface_protocol(dev);
    h->iface = usb_dev_interface_number(dev);
    if (h->iface < 0)
        h->iface = 0;
    memset(h->last, 0, sizeof(h->last));
    h->in_use = true;

    DEBUG_PRINT(h->proto == HID_PROTO_KEYBOARD ? "[usb_hid] claimed keyboard\r\n"
                : h->proto == HID_PROTO_MOUSE  ? "[usb_hid] claimed mouse\r\n"
                                               : "[usb_hid] claimed HID device\r\n");

    // Select boot protocol and disable idle (report only on change). Best-effort.
    usb_setup_packet_t set_proto = {0x21, HID_REQ_SET_PROTOCOL, 0 /*boot*/, (uint16_t)h->iface, 0};
    usb_dev_control(dev, &set_proto, NULL, 0);
    usb_setup_packet_t set_idle = {0x21, HID_REQ_SET_IDLE, 0, (uint16_t)h->iface, 0};
    usb_dev_control(dev, &set_idle, NULL, 0);

    cs_id task = 0;
    if (create_task_kernel("usb_hid_poll", task_permissions_kernel, &task) != CS_OK)
        return -1;
    start_task_kernel(task, hid_poll_task, h);
    return 0;
}

int module_init() {
    memset(hid_devs, 0, sizeof(hid_devs));
    usb_register_class_driver(USB_CLASS_HID, hid_probe);
    DEBUG_PRINT("[usb_hid] registered HID class driver\r\n");
    return 0;
}
