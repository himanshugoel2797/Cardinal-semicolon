// Copyright (c) 2021 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_USB_DRIV_H
#define CARDINALSEMI_USB_DRIV_H

#include <stdint.h>
#include <types.h>

typedef enum
{
    usb_device_type_unknown = 0,
    usb_device_type_uhci = 1,
    usb_device_type_ehci = 2,
    usb_device_type_ohci = 3,
    usb_device_type_xhci = 4,

    usb_device_type_hub = 5,

    usb_device_type_count,
} usb_device_type_t;

typedef enum
{
    usb_device_state_uninitialized,
    usb_device_state_disconnecting,
    usb_device_state_driver_disconnected,
} usb_device_state_t;

// ---------------------------------------------------------------------------
// USB wire structures and constants (USB 2.0 ch. 9).
// ---------------------------------------------------------------------------
typedef enum {
    usb_speed_low = 0,
    usb_speed_full = 1,
    usb_speed_high = 2,
    usb_speed_super = 3,
} usb_speed_t;

// bmRequestType bits
#define USB_REQ_DIR_IN   0x80
#define USB_REQ_DIR_OUT  0x00
#define USB_REQ_TYPE_STANDARD 0x00
#define USB_REQ_RECIP_DEVICE 0x00
#define USB_REQ_RECIP_INTERFACE 0x01

// bRequest (standard)
#define USB_REQ_GET_STATUS 0
#define USB_REQ_CLEAR_FEATURE 1
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE 11

// wValue descriptor types (high byte)
#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIG 2
#define USB_DESC_STRING 3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5

// Interface bInterfaceClass values we care about
#define USB_CLASS_HID 0x03
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB 0x09

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} PACKED usb_setup_packet_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} PACKED usb_device_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} PACKED usb_config_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} PACKED usb_interface_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;  // bit7 = IN
    uint8_t bmAttributes;      // bits1:0 = transfer type (2=bulk,3=interrupt)
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} PACKED usb_endpoint_descriptor_t;

// ---------------------------------------------------------------------------
// Host-controller transfer backend. A host-controller driver fills these in on
// its usb_hci_desc_t; CoreUsb calls them (passing the HC's own `state`) to drive
// enumeration and to service class drivers. All are SYNCHRONOUS for this first
// cut (return bytes transferred, or negative on error/timeout/STALL).
//
// NOTE: this transfer interface is a first, deliberately-simple cut intended to
// be reviewed and likely redesigned (async/event-driven, zero-copy, etc.). See
// notes/drivers/uhci-enumeration.md.
// ---------------------------------------------------------------------------
typedef struct {
    int (*control)(void *hc_state, int dev_addr, usb_speed_t speed, int max_packet0,
                   const usb_setup_packet_t *setup, void *data, int data_len);
    int (*interrupt_in)(void *hc_state, int dev_addr, usb_speed_t speed, int endpoint,
                        int max_packet, int data_toggle, void *data, int len);
    int (*bulk)(void *hc_state, int dev_addr, int endpoint, int max_packet,
                int data_toggle, void *data, int len, int dir_in);
} usb_hci_handlers_t;

typedef struct {
    char name[256];

    void *state;

    usb_device_type_t device_type;

    usb_hci_handlers_t handlers;

    int lock;
} usb_hci_desc_t;

typedef struct {
    char name[256];
    uint32_t hci_id;
    uint32_t address;
    uint32_t port;
    uint32_t speed;

    int lock;
} usb_device_t;

int usb_register_hostcontroller(usb_hci_desc_t *desc, void **handle);
int usb_register_device(usb_device_t *desc, void **handle);
int usb_remove_device(void *handle);

// ---------------------------------------------------------------------------
// Enumeration + class-driver interface (CoreUsb).
// ---------------------------------------------------------------------------

// An enumerated device handed to a class driver's probe callback. Opaque-ish:
// class drivers use the accessor transfer calls below with this handle.
typedef struct usb_enum_device usb_enum_device_t;

// A class driver registers a probe for a given bInterfaceClass. CoreUsb calls it
// after enumerating a device whose (first) interface matches `dev_class`. Return
// 0 if the driver claimed the device.
typedef int (*usb_class_probe_t)(usb_enum_device_t *dev);
int usb_register_class_driver(uint8_t dev_class, usb_class_probe_t probe);

// Called by a host-controller driver when a (reset, enabled) port reports a new
// device. CoreUsb performs address assignment + descriptor reads, then dispatches
// to a matching class driver. `hc_handle` is what usb_register_hostcontroller
// returned. Returns 0 on success.
int usb_port_connected(void *hc_handle, int port, usb_speed_t speed);

// Transfer helpers usable by a class driver against an enumerated device.
int usb_dev_control(usb_enum_device_t *dev, const usb_setup_packet_t *setup,
                    void *data, int data_len);
int usb_dev_interrupt_in(usb_enum_device_t *dev, int endpoint, int max_packet,
                         void *data, int len);
int usb_dev_bulk(usb_enum_device_t *dev, int endpoint, int max_packet,
                 void *data, int len, int dir_in);

// Accessors for class drivers.
const usb_device_descriptor_t *usb_dev_descriptor(usb_enum_device_t *dev);
int usb_dev_address(usb_enum_device_t *dev);
usb_speed_t usb_dev_speed(usb_enum_device_t *dev);
// Find the first endpoint of a given type (2=bulk,3=interrupt) and direction
// (dir_in!=0 for IN) in the active configuration. Returns the endpoint address
// (incl. dir bit) or -1; writes max packet size to *max_packet if non-NULL.
int usb_dev_find_endpoint(usb_enum_device_t *dev, int type, int dir_in, int *max_packet);
// First interface's bInterfaceProtocol/bInterfaceNumber (or -1). For HID boot
// devices protocol 1 = keyboard, 2 = mouse.
int usb_dev_interface_protocol(usb_enum_device_t *dev);
int usb_dev_interface_number(usb_enum_device_t *dev);
// For a hub driver: enumerate a device on one of the hub's downstream ports.
int usb_dev_enumerate_downstream(usb_enum_device_t *hub, int port, usb_speed_t speed);

#endif
