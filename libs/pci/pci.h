// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PCI_LIB_H
#define CARDINAL_PCI_LIB_H

#include <stdint.h>
#include <types.h>

typedef struct {
    uint16_t unkn0 : 2;
    uint16_t busmaster : 1;
    uint16_t unkn1 : 13;
} pci_command_reg_t;

typedef struct PACKED {
    uint16_t vendorID;
    uint16_t deviceID;
    pci_command_reg_t command;
    uint16_t status;
    uint32_t revisionID : 8;
    uint32_t classCode : 24;
    uint8_t cacheLine;
    uint8_t masterLatency;
    uint8_t headerType;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbus;
    uint16_t subsystem_vendor;
    uint16_t subsystem;
    uint32_t expansion_rom;
    uint32_t capabilitiesPtr : 8;
    uint32_t rsv0 : 24;
} pci_config_t;

typedef enum {
    pci_cap_pwm = 0x01,
    pci_cap_msi = 0x05,
    pci_cap_vendor = 0x09,
    pci_cap_msix = 0x11,
} pci_cap_ids_t;

typedef struct PACKED {
    uint8_t capID;
    uint8_t nextPtr;
    uint8_t data[0];
} pci_cap_header_t;

typedef struct {
    uint16_t enable : 1;
    uint16_t requested_vector_num : 3;
    uint16_t avail_vector_num : 3;
    uint16_t support_64bit : 1;
    uint16_t support_vectormask : 1;
} pci_msi_control_t;

typedef struct {
    pci_cap_header_t hdr;
    pci_msi_control_t ctrl;
    uint32_t msg_addr;
    uint16_t msg_data;
} pci_msi_32_t;

typedef struct {
    pci_cap_header_t hdr;
    pci_msi_control_t ctrl;
    uint32_t msg_addr;
    uint32_t msg_addr_hi;
    uint16_t msg_data;
} pci_msi_64_t;


static inline void pci_getmsiinfo(pci_config_t *device) {
    //Search cap list
    if(device->capabilitiesPtr != 0) {
        uint8_t ptr = device->capabilitiesPtr;
        uint8_t *pci_base = (uint8_t*)device;

        do {
            pci_cap_header_t *capEntry = (pci_cap_header_t*)(pci_base + ptr);

            if(capEntry->capID == pci_cap_msi) {

            } else if(capEntry->capID == pci_cap_msix) {

            } else
                ptr = capEntry->nextPtr;
        } while(ptr != 0);
    }
}

#endif