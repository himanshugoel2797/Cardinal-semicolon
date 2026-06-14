/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB hub driver. Registers as a CoreUsb class driver for bInterfaceClass==Hub;
 * on probe it reads the hub descriptor, powers the ports, and polls for
 * downstream connections, resetting and enumerating each new device (which then
 * routes to its own class driver). Downstream devices live on the same host
 * controller as the hub, so enumeration just runs normally per-device.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "CoreUsb/usb.h"
#include "SysTaskMgr/task.h"

#define HUB_DESC_TYPE 0x29

// Hub class request recipients/values.
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET 20

// wPortStatus bits.
#define PS_CONNECTION (1 << 0)
#define PS_ENABLE (1 << 1)
#define PS_LOW_SPEED (1 << 9)

#define HUB_MAX_PORTS 15

typedef struct {
    usb_enum_device_t *dev;
    int nports;
    bool port_enum[HUB_MAX_PORTS + 1];  // 1-based
    bool in_use;
} hub_dev_t;

#define MAX_HUB 4
static hub_dev_t hubs[MAX_HUB];

static void hub_delay(uint64_t iters) {
    for (volatile uint64_t i = 0; i < iters; i++)
        ;
}

static int hub_get_status(hub_dev_t *h, int port, uint16_t *status, uint16_t *change) {
    uint8_t buf[4];
    usb_setup_packet_t s = {0xA3, 0 /*GET_STATUS*/, 0, (uint16_t)port, 4};
    if (usb_dev_control(h->dev, &s, buf, 4) < 4)
        return -1;
    if (status)
        *status = (uint16_t)(buf[0] | (buf[1] << 8));
    if (change)
        *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return 0;
}

static void hub_set_feature(hub_dev_t *h, int port, int feature) {
    usb_setup_packet_t s = {0x23, 3 /*SET_FEATURE*/, (uint16_t)feature, (uint16_t)port, 0};
    usb_dev_control(h->dev, &s, NULL, 0);
}

static void hub_clear_feature(hub_dev_t *h, int port, int feature) {
    usb_setup_packet_t s = {0x23, 1 /*CLEAR_FEATURE*/, (uint16_t)feature, (uint16_t)port, 0};
    usb_dev_control(h->dev, &s, NULL, 0);
}

static void hub_task(void *arg) {
    hub_dev_t *h = (hub_dev_t *)arg;
    DEBUG_PRINT("[usb_hub] polling ports\r\n");

    while (true) {
        for (int port = 1; port <= h->nports; port++) {
            uint16_t status = 0, change = 0;
            if (hub_get_status(h, port, &status, &change) < 0)
                continue;

            bool connected = (status & PS_CONNECTION) != 0;
            if (connected && !h->port_enum[port]) {
                h->port_enum[port] = true;
                DEBUG_PRINT("[usb_hub] downstream device connected; resetting\r\n");
                hub_clear_feature(h, port, C_PORT_CONNECTION);
                hub_set_feature(h, port, PORT_RESET);
                hub_delay(40000000);  // ~reset duration
                hub_clear_feature(h, port, C_PORT_RESET);
                hub_delay(20000000);  // recovery

                if (hub_get_status(h, port, &status, &change) < 0)
                    continue;
                if (!(status & PS_ENABLE))
                    continue;
                usb_speed_t spd = (status & PS_LOW_SPEED) ? usb_speed_low : usb_speed_full;
                usb_dev_enumerate_downstream(h->dev, port, spd);
            } else if (!connected && h->port_enum[port]) {
                h->port_enum[port] = false;
                DEBUG_PRINT("[usb_hub] downstream device disconnected\r\n");
            }
        }
        task_yield();
    }
}

static int hub_probe(usb_enum_device_t *dev) {
    // Read the hub descriptor (class GET_DESCRIPTOR, type 0x29). 8 bytes is
    // enough to reach bNbrPorts (offset 2).
    uint8_t desc[16];
    memset(desc, 0, sizeof(desc));
    usb_setup_packet_t s = {0xA0, USB_REQ_GET_DESCRIPTOR, (uint16_t)(HUB_DESC_TYPE << 8), 0, 8};
    if (usb_dev_control(dev, &s, desc, 8) < 8) {
        DEBUG_PRINT("[usb_hub] hub descriptor read failed; not claiming\r\n");
        return -1;
    }
    int nports = desc[2];
    if (nports > HUB_MAX_PORTS)
        nports = HUB_MAX_PORTS;

    hub_dev_t *h = NULL;
    for (int i = 0; i < MAX_HUB; i++)
        if (!hubs[i].in_use) {
            h = &hubs[i];
            break;
        }
    if (h == NULL)
        return -1;

    memset(h, 0, sizeof(*h));
    h->dev = dev;
    h->nports = nports;
    h->in_use = true;

    char b[8];
    DEBUG_PRINT("[usb_hub] claimed hub with ");
    DEBUG_PRINT(itoa(nports, b, 10));
    DEBUG_PRINT(" ports\r\n");

    // Power on every port, then let them settle.
    for (int port = 1; port <= nports; port++)
        hub_set_feature(h, port, PORT_POWER);
    hub_delay(40000000);

    cs_id task = 0;
    if (create_task_kernel("usb_hub_poll", task_permissions_kernel, &task) != CS_OK)
        return -1;
    start_task_kernel(task, hub_task, h);
    return 0;
}

int module_init() {
    memset(hubs, 0, sizeof(hubs));
    usb_register_class_driver(USB_CLASS_HUB, hub_probe);
    DEBUG_PRINT("[usb_hub] registered hub class driver\r\n");
    return 0;
}
