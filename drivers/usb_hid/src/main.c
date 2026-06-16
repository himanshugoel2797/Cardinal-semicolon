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
 * Keyboards register with CoreInput via input_device_register and feed it
 * decoded key events. Mouse motion needs float axes that kernel modules can't
 * use (-mno-sse), so the mouse is decoded to the debug console only for now.
 * Decoded reports are also echoed to the debug console for bring-up visibility.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include <cardinal/local_spinlock.h>

#include "CoreUsb/usb.h"
#include "CoreInput/input.h"
#include "SysTaskMgr/task.h"

#define HID_PROTO_KEYBOARD 1
#define HID_PROTO_MOUSE 2

// HID class requests.
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

#define HID_EVQ_SIZE 64

typedef struct {
    usb_enum_device_t *dev;
    int endpoint;
    int max_packet;
    int proto;
    int iface;
    bool in_use;
    cs_id task;
    volatile bool stop;     // remove() requests the poll task to exit
    volatile bool stopped;  // poll task acknowledges it has exited
    uint8_t last[8];

    // Event queue feeding CoreInput (keyboard only). SPSC: HID poll task pushes,
    // CoreInput's reader pops; guarded by a spinlock for safety.
    input_device_event_t evq[HID_EVQ_SIZE];
    int ev_head, ev_tail;
    int ev_lock;
    input_device_desc_t input_desc;
} hid_dev_t;

// Map a HID keyboard usage code to a CoreInput key index. Letters map directly;
// everything else is reported as kbd_keys_unkn (the raw usage is still in the
// debug log). Extend as needed.
static uint32_t hid_usage_to_kbd(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1D)
        return (uint32_t)kbd_keys_A + (usage - 0x04);
    return (uint32_t)kbd_keys_unkn;
}

static void hid_push_event(hid_dev_t *h, uint32_t index, bool down) {
    local_spinlock_lock(&h->ev_lock);
    int next = (h->ev_head + 1) % HID_EVQ_SIZE;
    if (next != h->ev_tail) {  // drop if full
        input_device_event_t *e = &h->evq[h->ev_head];
        e->timestamp = 0;
        e->is_btn_event = true;
        e->index = index;
        e->state = down;
        h->ev_head = next;
    }
    local_spinlock_unlock(&h->ev_lock);
}

static bool hid_has_pending(void *state) {
    hid_dev_t *h = (hid_dev_t *)state;
    return h->ev_head != h->ev_tail;
}

static void hid_read_event(void *state, input_device_event_t *out) {
    hid_dev_t *h = (hid_dev_t *)state;
    local_spinlock_lock(&h->ev_lock);
    if (h->ev_tail != h->ev_head) {
        *out = h->evq[h->ev_tail];
        h->ev_tail = (h->ev_tail + 1) % HID_EVQ_SIZE;
    } else {
        memset(out, 0, sizeof(*out));
    }
    local_spinlock_unlock(&h->ev_lock);
}

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
    // rpt[0]=modifiers, rpt[2..7]=keycodes (currently-down set). Diff against the
    // previous report to emit key-down / key-up events to CoreInput.
    for (int i = 2; i < 8; i++) {  // newly pressed
        uint8_t k = rpt[i];
        if (k == 0)
            continue;
        bool was_down = false;
        for (int j = 2; j < 8; j++)
            if (h->last[j] == k)
                was_down = true;
        if (!was_down) {
            hid_push_event(h, hid_usage_to_kbd(k), true);
            char c = hid_keycode_to_char(k), buf[8];
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
    for (int i = 2; i < 8; i++) {  // released
        uint8_t k = h->last[i];
        if (k == 0)
            continue;
        bool still_down = false;
        for (int j = 2; j < 8; j++)
            if (rpt[j] == k)
                still_down = true;
        if (!still_down)
            hid_push_event(h, hid_usage_to_kbd(k), false);
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
    while (!h->stop) {
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
    h->stopped = true;  // hand off: remove() may now reclaim h->dev
    end_task_kernel(task_current());
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
    h->stop = false;
    h->stopped = false;
    h->in_use = true;

    DEBUG_PRINT(h->proto == HID_PROTO_KEYBOARD ? "[usb_hid] claimed keyboard\r\n"
                : h->proto == HID_PROTO_MOUSE  ? "[usb_hid] claimed mouse\r\n"
                                               : "[usb_hid] claimed HID device\r\n");

    // Select boot protocol and disable idle (report only on change). Best-effort.
    usb_setup_packet_t set_proto = {0x21, HID_REQ_SET_PROTOCOL, 0 /*boot*/, (uint16_t)h->iface, 0};
    usb_dev_control(dev, &set_proto, NULL, 0);
    usb_setup_packet_t set_idle = {0x21, HID_REQ_SET_IDLE, 0, (uint16_t)h->iface, 0};
    usb_dev_control(dev, &set_idle, NULL, 0);

    // Register a keyboard with CoreInput (real input integration). Mouse motion
    // needs float axes which kernel modules can't use (-mno-sse), so the mouse
    // stays debug-print only for now.
    if (h->proto == HID_PROTO_KEYBOARD) {
        h->ev_head = h->ev_tail = 0;
        h->ev_lock = 0;
        memset(&h->input_desc, 0, sizeof(h->input_desc));
        strncpy(h->input_desc.name, "USB Keyboard", 32);
        h->input_desc.type = input_device_type_keyboard;
        h->input_desc.features = input_device_features_none;
        h->input_desc.handlers.has_pending = hid_has_pending;
        h->input_desc.handlers.read = hid_read_event;
        h->input_desc.state = h;
        input_device_register(&h->input_desc);
        DEBUG_PRINT("[usb_hid] registered keyboard with CoreInput\r\n");
    }

    h->task = 0;
    if (create_task_kernel("usb_hid_poll", task_permissions_kernel, &h->task) != CS_OK) {
        if (h->proto == HID_PROTO_KEYBOARD)
            input_device_unregister(&h->input_desc);
        h->in_use = false;
        return -1;
    }
    start_task_kernel(h->task, hid_poll_task, h);
    return 0;
}

static void hid_remove(usb_enum_device_t *dev) {
    for (int i = 0; i < MAX_HID; i++) {
        hid_dev_t *h = &hid_devs[i];
        if (!h->in_use || h->dev != dev)
            continue;
        // Stop the poll task and wait until it has actually exited, so it never
        // touches `dev` after CoreUsb reclaims it.
        h->stop = true;
        while (!h->stopped)
            task_yield();
        if (h->proto == HID_PROTO_KEYBOARD)
            input_device_unregister(&h->input_desc);
        h->in_use = false;
        DEBUG_PRINT("[usb_hid] device removed\r\n");
    }
}

int module_init() {
    memset(hid_devs, 0, sizeof(hid_devs));
    usb_register_class_driver(USB_CLASS_HID, hid_probe, hid_remove);
    DEBUG_PRINT("[usb_hid] registered HID class driver\r\n");
    return 0;
}
