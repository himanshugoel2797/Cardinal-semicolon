/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB enumeration + class-driver dispatch. Controller-agnostic: it drives a
 * device through address assignment and descriptor reads using the host
 * controller's synchronous transfer handlers, then hands the device to a class
 * driver that registered for its interface class.
 *
 * This is a first cut meant to be reviewed/redesigned (see
 * notes/drivers/uhci-enumeration.md and notes/servers/CoreUsb-roadmap.md).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "CoreUsb/usb.h"
#include "usb_priv.h"
#include "SysTimer/timer.h"

struct usb_enum_device {
    usb_hci_desc_t *hc;
    int address;
    usb_speed_t speed;
    int max_packet0;
    bool in_use;
    usb_device_descriptor_t dev_desc;
    uint8_t config_buf[512];
    int config_len;
};

#define MAX_ENUM_DEVICES 32
static struct usb_enum_device enum_devices[MAX_ENUM_DEVICES];
static int enum_lock = 0;

static int next_address = 1;  // 1..127, 0 is the default address

static usb_class_probe_t class_probes[256] = {0};

// Bounded busy-wait (timer_timestamp_ns is unreliable in this context -- it uses
// floating point and a possibly-absent counter timer). Not precise; "at least
// roughly this long" is all the SET_ADDRESS recovery needs.
static void usb_delay_ns(uint64_t ns) {
    for (volatile uint64_t i = 0; i < ns; i++)
        ;
}

int usb_register_class_driver(uint8_t dev_class, usb_class_probe_t probe) {
    class_probes[dev_class] = probe;
    DEBUG_PRINT("[CoreUsb] Registered class driver\r\n");
    return 0;
}

// ---- transfer helpers exposed to class drivers ----
int usb_dev_control(usb_enum_device_t *dev, const usb_setup_packet_t *setup,
                    void *data, int data_len) {
    if (dev->hc->handlers.control == NULL)
        return -1;
    return dev->hc->handlers.control(dev->hc->state, dev->address, dev->speed,
                                     dev->max_packet0, setup, data, data_len);
}

int usb_dev_interrupt_in(usb_enum_device_t *dev, int endpoint, int max_packet,
                         void *data, int len) {
    if (dev->hc->handlers.interrupt_in == NULL)
        return -1;
    // Toggle state is owned by the caller of the lower layer; for simple polled
    // HID we restart at toggle 0 each call -- adequate for boot-protocol reads
    // where we just want the latest report. (A real driver tracks the toggle.)
    return dev->hc->handlers.interrupt_in(dev->hc->state, dev->address, dev->speed,
                                          endpoint, max_packet, 0, data, len);
}

int usb_dev_bulk(usb_enum_device_t *dev, int endpoint, int max_packet,
                 void *data, int len, int dir_in) {
    if (dev->hc->handlers.bulk == NULL)
        return -1;
    return dev->hc->handlers.bulk(dev->hc->state, dev->address, endpoint,
                                  max_packet, 0, data, len, dir_in);
}

const usb_device_descriptor_t *usb_dev_descriptor(usb_enum_device_t *dev) { return &dev->dev_desc; }
int usb_dev_address(usb_enum_device_t *dev) { return dev->address; }
usb_speed_t usb_dev_speed(usb_enum_device_t *dev) { return dev->speed; }

int usb_dev_find_endpoint(usb_enum_device_t *dev, int type, int dir_in, int *max_packet) {
    int off = 0;
    while (off + 2 <= dev->config_len) {
        uint8_t len = dev->config_buf[off];
        uint8_t dtype = dev->config_buf[off + 1];
        if (len == 0)
            break;
        if (dtype == USB_DESC_ENDPOINT && off + (int)sizeof(usb_endpoint_descriptor_t) <= dev->config_len) {
            usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)&dev->config_buf[off];
            if ((ep->bmAttributes & 0x3) == type && (!!(ep->bEndpointAddress & 0x80)) == (!!dir_in)) {
                if (max_packet != NULL)
                    *max_packet = ep->wMaxPacketSize;
                return ep->bEndpointAddress;
            }
        }
        off += len;
    }
    return -1;
}

static usb_enum_device_t *alloc_enum_device(void) {
    local_spinlock_lock(&enum_lock);
    usb_enum_device_t *r = NULL;
    for (int i = 0; i < MAX_ENUM_DEVICES; i++)
        if (!enum_devices[i].in_use) {
            enum_devices[i].in_use = true;
            r = &enum_devices[i];
            break;
        }
    local_spinlock_unlock(&enum_lock);
    return r;
}

static void print_hex16(const char *label, uint16_t v) {
    char tmp[8];
    DEBUG_PRINT(label);
    DEBUG_PRINT(itoa(v, tmp, 16));
    DEBUG_PRINT(" ");
}

int usb_port_connected(void *hc_handle, int port, usb_speed_t speed) {
    usb_hci_def_t *def = (usb_hci_def_t *)hc_handle;
    usb_hci_desc_t *hc = &def->device;
    (void)port;

    if (hc->handlers.control == NULL) {
        DEBUG_PRINT("[CoreUsb] HC has no control handler; cannot enumerate\r\n");
        return -1;
    }

    // 1) Read the first 8 bytes of the device descriptor at the default address
    //    (max packet size for ep0 is unknown -> the spec-guaranteed minimum, 8).
    usb_setup_packet_t s = {USB_REQ_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                            (uint16_t)(USB_DESC_DEVICE << 8), 0, 8};
    uint8_t buf8[8];
    int r = hc->handlers.control(hc->state, 0, speed, 8, &s, buf8, 8);
    if (r < 8) {
        DEBUG_PRINT("[CoreUsb] enum: initial GET_DESCRIPTOR failed\r\n");
        return -1;
    }
    int mps0 = buf8[7];
    if (mps0 == 0)
        mps0 = 8;

    // 2) Assign an address.
    local_spinlock_lock(&enum_lock);
    int addr = next_address++;
    local_spinlock_unlock(&enum_lock);
    usb_setup_packet_t sa = {USB_REQ_DIR_OUT, USB_REQ_SET_ADDRESS, (uint16_t)addr, 0, 0};
    r = hc->handlers.control(hc->state, 0, speed, mps0, &sa, NULL, 0);
    if (r < 0) {
        DEBUG_PRINT("[CoreUsb] enum: SET_ADDRESS failed\r\n");
        return -1;
    }
    usb_delay_ns(5 * 1000 * 1000);  // >=2ms for the device to switch address

    usb_enum_device_t *dev = alloc_enum_device();
    if (dev == NULL)
        return -1;
    dev->hc = hc;
    dev->address = addr;
    dev->speed = speed;
    dev->max_packet0 = mps0;
    dev->config_len = 0;

    // 3) Full device descriptor at the new address.
    usb_setup_packet_t sd = {USB_REQ_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_DEVICE << 8), 0, sizeof(usb_device_descriptor_t)};
    r = usb_dev_control(dev, &sd, &dev->dev_desc, sizeof(usb_device_descriptor_t));
    if (r < (int)sizeof(usb_device_descriptor_t)) {
        DEBUG_PRINT("[CoreUsb] enum: device descriptor read failed\r\n");
        dev->in_use = false;
        return -1;
    }

    DEBUG_PRINT("[CoreUsb] Enumerated device: ");
    print_hex16("VID=", dev->dev_desc.idVendor);
    print_hex16("PID=", dev->dev_desc.idProduct);
    print_hex16("class=", dev->dev_desc.bDeviceClass);
    DEBUG_PRINT("\r\n");

    // 4) Configuration descriptor: 9 bytes to learn wTotalLength, then the whole.
    usb_config_descriptor_t cfg;
    usb_setup_packet_t sc = {USB_REQ_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_CONFIG << 8), 0, sizeof(usb_config_descriptor_t)};
    r = usb_dev_control(dev, &sc, &cfg, sizeof(usb_config_descriptor_t));
    if (r < (int)sizeof(usb_config_descriptor_t)) {
        DEBUG_PRINT("[CoreUsb] enum: config descriptor (short) read failed\r\n");
        dev->in_use = false;
        return -1;
    }
    int total = cfg.wTotalLength;
    if (total > (int)sizeof(dev->config_buf))
        total = sizeof(dev->config_buf);
    sc.wLength = (uint16_t)total;
    r = usb_dev_control(dev, &sc, dev->config_buf, total);
    if (r < total) {
        DEBUG_PRINT("[CoreUsb] enum: config descriptor (full) read failed\r\n");
        dev->in_use = false;
        return -1;
    }
    dev->config_len = total;

    // 5) Select the configuration.
    usb_setup_packet_t scfg = {USB_REQ_DIR_OUT, USB_REQ_SET_CONFIGURATION,
                               cfg.bConfigurationValue, 0, 0};
    r = usb_dev_control(dev, &scfg, NULL, 0);
    if (r < 0) {
        DEBUG_PRINT("[CoreUsb] enum: SET_CONFIGURATION failed\r\n");
        dev->in_use = false;
        return -1;
    }

    // 6) Find the first interface descriptor; dispatch to its class driver.
    int off = 0;
    uint8_t dev_class = dev->dev_desc.bDeviceClass;
    while (off + 2 <= dev->config_len) {
        uint8_t len = dev->config_buf[off];
        uint8_t dtype = dev->config_buf[off + 1];
        if (len == 0)
            break;
        if (dtype == USB_DESC_INTERFACE && off + (int)sizeof(usb_interface_descriptor_t) <= dev->config_len) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)&dev->config_buf[off];
            dev_class = iface->bInterfaceClass;
            break;
        }
        off += len;
    }

    if (class_probes[dev_class] != NULL) {
        DEBUG_PRINT("[CoreUsb] dispatching to class driver\r\n");
        class_probes[dev_class](dev);
    } else {
        DEBUG_PRINT("[CoreUsb] no class driver registered for this device\r\n");
    }

    return 0;
}
