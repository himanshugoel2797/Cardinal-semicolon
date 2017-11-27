/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

#include "registry.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

typedef struct {
    uint32_t ClassCode;
    uint32_t SubClassCode;
    uint32_t ProgIF;

    uint32_t HdrType;

    uint32_t DeviceID;
    uint32_t VendorID;

    uint32_t BarCount;

    uint32_t Bus;
    uint32_t Device;
    uint32_t Function;
} PCI_Device;

static uint32_t
PCI_ReadDWord(uint32_t bus,
              uint32_t device,
              uint32_t function,
              uint32_t offset) {
    outl(PCI_ADDR, 0x80000000 | bus << 16 | device << 11 | function <<  8 | (offset & 0xfc));
    return inl(PCI_DATA);
}

static void
PCI_WriteDWord(uint32_t bus,
               uint32_t device,
               uint32_t function,
               uint32_t offset,
               uint32_t val) {
    outl(PCI_ADDR, 0x80000000 | bus << 16 | device << 11 | function <<  8 | (offset & 0xfc));
    outl(PCI_DATA, val);
}

static int
PCI_GetNextDevice(uint32_t *bus,
                  uint32_t *device) {

    uint32_t b = *bus;
    uint32_t d = *device;

    for(; b < 256; b++)
        for(; d < 32; d++) {
            uint32_t v_id = PCI_ReadDWord(b, d, 0, 0);

            if(v_id != 0xFFFFFFFF) {
                *bus = b;
                *device = d;
                return 0;
            }
        }

    return -1;
}

static uint32_t
PCI_GetFuncCount(uint32_t bus,
                 uint32_t device) {
    uint32_t hdrType = PCI_ReadDWord(bus, device, 0, 0x0C);

    if((hdrType >> 23) & 1)
        return 8;

    return 1;
}

static void
PCI_GetPCIDevice(uint32_t bus,
                 uint32_t device,
                 uint32_t function,
                 PCI_Device *devInfo) {

    devInfo->ClassCode = PCI_ReadDWord(bus, device, function, 8) >> 24;
    devInfo->SubClassCode = (PCI_ReadDWord(bus, device, function, 8) >> 16) & 0xFF;
    devInfo->ProgIF = (PCI_ReadDWord(bus, device, function, 8) >> 8) & 0xFF;

    devInfo->HdrType = (PCI_ReadDWord(bus, device, function, 0x0C) >> 16) & 0xFF;

    devInfo->DeviceID = (uint16_t)(PCI_ReadDWord(bus, device, function, 0) >> 16);
    devInfo->VendorID = (uint16_t)PCI_ReadDWord(bus, device, function, 0);

    devInfo->Bus = bus;
    devInfo->Device = device;
    devInfo->Function = function;

    if(devInfo->HdrType == 0)
        devInfo->BarCount = 6;
    else
        devInfo->BarCount = 2;
}

static uint64_t
PCI_GetBAR(PCI_Device *device,
           uint32_t bar_index) {
    uint32_t l_word = PCI_ReadDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index * 4));

    if(l_word & 1) {
        return l_word & ~1;
    } else if(((l_word >> 1) & 3) == 0) {
        return l_word & ~0xF;
    } else if(((l_word >> 1) & 3) == 2) {
        return (l_word & ~0xF) | (uint64_t)(PCI_ReadDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index + 1) * 4)) << 32;
    }

    return l_word;
}

static uint32_t
PCI_IsIOSpaceBAR(PCI_Device *device,
                 uint32_t bar_index) {
    uint32_t l_word = PCI_ReadDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index * 4));

    return l_word & 1;
}

static uint64_t
PCI_GetBARSize(PCI_Device *device,
               uint32_t bar_index) {
    uint64_t bar_val = PCI_GetBAR(device, bar_index);

    PCI_WriteDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index * 4), 0xFFFFFFFF);
    uint32_t val = PCI_ReadDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index * 4));

    PCI_WriteDWord(device->Bus, device->Device, device->Function, 0x10 + (bar_index * 4), bar_val);

    return ~val + 1;
}

int pci_reg_init() {

    uint32_t bus = 0;
    uint32_t device = 0;
    uint32_t idx = 0;

    if(registry_createdirectory("HW", "PCI") != registry_err_ok)
        return -1;

    while(1) {
        if(PCI_GetNextDevice(&bus, &device) != 0)
            break;

        //Store device info
        uint32_t funcs = PCI_GetFuncCount(bus, device);
        for(uint32_t f = 0; f < funcs; f++) {
            char idx_str[10];
            char key_str[256] = "HW/PCI/";
            char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

            if(registry_createdirectory("HW/PCI", idx_str) != registry_err_ok)
                return -1;

            PCI_Device devInfo;
            PCI_GetPCIDevice(bus, device, f, &devInfo);

            if(registry_addkey_uint(key_idx, "BUS", bus) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "DEVICE", device) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "FUNCTION", f) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "CLASS", devInfo.ClassCode) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "SUBCLASS", devInfo.SubClassCode) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "DEVICE_ID", devInfo.DeviceID) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "VENDOR_ID", devInfo.VendorID) != registry_err_ok)
                return -1;

            if(registry_addkey_uint(key_idx, "BAR_COUNT", devInfo.BarCount) != registry_err_ok)
                return -1;

            //Parse and store BARs
            for(uint32_t b0 = 0; b0 < devInfo.BarCount; b0++) {
                char idx2_str[10];
                char key2_str[256];
                char *key2_idx = strncpy(key2_str, key_str, 255);
                itoa(b0, idx2_str, 16);

                if(registry_createdirectory(key2_idx, idx2_str) != registry_err_ok)
                    return -1;

                key2_idx = strncat(key2_str, "/", 255);
                key2_idx = strncat(key2_str, idx2_str, 255);

                uint64_t bar_val = PCI_GetBAR(&devInfo, b0);
                uint64_t bar_sz = PCI_GetBARSize(&devInfo, b0);
                bool isIOspace = PCI_IsIOSpaceBAR(&devInfo, b0);

                if(registry_addkey_uint(key2_idx, "ADDRESS", bar_val) != registry_err_ok)
                    return -1;

                if(registry_addkey_uint(key2_idx, "SIZE", bar_sz) != registry_err_ok)
                    return -1;

                if(registry_addkey_bool(key2_idx, "IS_IO", isIOspace) != registry_err_ok)
                    return -1;
            }
        }

        device++;
        idx++;
    }

    return 0;
}