// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PCI_LIB_H
#define CARDINAL_PCI_LIB_H

#include <stdint.h>
#include <types.h>
#include "SysVirtualMemory/vmem.h"

typedef struct {
    uint16_t unkn0 : 1;
    uint16_t mem_space : 1;
    uint16_t busmaster : 1;
    uint16_t unkn1 : 7;
    uint16_t int_disable : 1;
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
    uint16_t rsv;
} pci_msi_32_t;

typedef struct {
    pci_cap_header_t hdr;
    pci_msi_control_t ctrl;
    uint32_t msg_addr;
    uint32_t msg_addr_hi;
    uint16_t msg_data;
    uint16_t rsv;
} pci_msi_64_t;

typedef struct {
    uint16_t table_sz : 11;
    uint16_t rsv0 : 3;
    uint16_t func_mask : 1;
    uint16_t enable : 1;
} pci_msix_control_t;

typedef struct {
    uint32_t bir : 3;
    uint32_t offset : 29;
} pci_msix_entry_t;

typedef struct {
    pci_cap_header_t hdr;
    pci_msix_control_t ctrl;
    pci_msix_entry_t table_off;
    pci_msix_entry_t pba_off;
} pci_msix_t;

static inline int pci_getmsiinfo(pci_config_t *device, int *cnt) {
    //Search cap list
    bool hasmsi = false;
    bool hasmsix = false;
    if(device->capabilitiesPtr != 0) {
        uint8_t ptr = device->capabilitiesPtr;
        uint8_t *pci_base = (uint8_t*)device;

        do {
            pci_cap_header_t *capEntry = (pci_cap_header_t*)(pci_base + ptr);

            if(capEntry->capID == pci_cap_msi) {
                hasmsi = true;

                pci_msi_32_t *msi_space = (pci_msi_32_t*)capEntry;
                *cnt = 1 << msi_space->ctrl.requested_vector_num;

            } else if(capEntry->capID == pci_cap_msix) {
                hasmsi = true;
                hasmsix = true;

                pci_msix_t *msi_space = (pci_msix_t*)capEntry;
                *cnt = msi_space->ctrl.table_sz + 1;

            }
            ptr = capEntry->nextPtr;
        } while(ptr != 0);
    }

    if(hasmsix) return 1;
    if(hasmsi) return 0;
    return -1;
}

static inline int pci_setmsiinfo(pci_config_t *device, int msix, uintptr_t *msi_addr, uint32_t *msi_msg, int cnt) {
    //Search cap list
    if(device->capabilitiesPtr != 0) {
        uint8_t ptr = device->capabilitiesPtr;
        uint8_t *pci_base = (uint8_t*)device;

        do {
            pci_cap_header_t *capEntry = (pci_cap_header_t*)(pci_base + ptr);

            if(capEntry->capID == pci_cap_msi && msix == 0) {

                pci_msi_32_t *msi_space = (pci_msi_32_t*)capEntry;
                if(msi_space->ctrl.support_64bit) {
                    pci_msi_64_t *msi64_space = (pci_msi_64_t*)capEntry;
                    msi64_space->msg_addr = (uint32_t) *msi_addr;
                    msi64_space->msg_addr_hi = (uint32_t) (*msi_addr >> 32);
                    msi64_space->msg_data = *msi_msg;
                } else {
                    msi_space->msg_addr = (uint32_t) *msi_addr;
                    msi_space->msg_data = *msi_msg;
                }

                uint32_t coded_vecnum = 0;
                for(int i = 30; i >= 0; i--)
                    if(cnt & (1 << i)) {
                        coded_vecnum = i;
                        break;
                    }

                msi_space->ctrl.avail_vector_num = coded_vecnum;
                msi_space->ctrl.enable = 1;

            } else if(capEntry->capID == pci_cap_msix && msix == 1) {

                pci_msix_t *msi_space = (pci_msix_t*)capEntry;

                if(cnt != 1 && (msi_space->ctrl.table_sz + 1) != cnt)
                    return -1;

                //get the specified bar, get its virtual address
                uint64_t bar = 0;
                for(int i = 0; i < 6; i++) {
                    if((device->bar[i] & 0x6) == 0x4) //Is 64-bit
                        bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
                    else if((device->bar[i] & 0x6) == 0x0) //Is 32-bit
                        bar = (device->bar[i] & 0xFFFFFFF0);
                    if(i == msi_space->table_off.bir) break;
                }

                uint32_t *table = (uint32_t*)vmem_phystovirt((intptr_t)(bar + (msi_space->table_off.offset << 3)), KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

                //fill the offset table
                for(int i = 0; i <= msi_space->ctrl.table_sz; i++) {
                    table[i * 4 + 0] = (uint32_t)msi_addr[ cnt == 1 ? 0 : i];
                    table[i * 4 + 1] = (uint32_t)(msi_addr[cnt == 1 ? 0 : i] >> 32);
                    table[i * 4 + 2] = msi_msg[cnt == 1 ? 0 : i];
                    table[i * 4 + 3] &= ~1;
                }

                msi_space->ctrl.func_mask = 0;
                msi_space->ctrl.enable = 1;
                //__asm__("cli\n\thlt" :: "a"(table));
            }
            ptr = capEntry->nextPtr;
        } while(ptr != 0);
    }

    return 0;
}

#endif