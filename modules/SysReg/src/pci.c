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

#include "acpi/acpi_tables.h"
#include "acpi/mcfg.h"
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

static int
PCI_GetNextDevice(uint32_t *bus,
                  uint32_t *device) {

    uint32_t b = *bus;
    uint32_t d = *device;


    while(b < 256) {
        for(; d < 32; d++) {
            uint32_t v_id = PCI_ReadDWord(b, d, 0, 0);

            if(v_id != 0xFFFFFFFF) {
                *bus = b;
                *device = d;
                return 0;
            }
        }

        if(d >= 32) {
            d = 0;
            b++;
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

int pci_reg_init() {

    uint32_t bus = 0;
    uint32_t device = 0;
    uint32_t idx = 0;

    if(registry_createdirectory("HW", "PCI") != registry_err_ok)
        return -1;

    //Find the ECAM address if possible
    MCFG* mcfg = ACPITables_FindTable(MCFG_SIG);
    uint32_t len = mcfg->h.Length - 8 - sizeof(ACPISDTHeader);

    while(1) {
        if(PCI_GetNextDevice(&bus, &device) != 0)
            break;

        //Store device info
        uint32_t funcs = PCI_GetFuncCount(bus, device);
        for(uint32_t f = 0; f < funcs; f++) {
            char idx_str[10] = "";
            char key_str[256] = "HW/PCI/";
            char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

            PCI_Device devInfo;
            PCI_GetPCIDevice(bus, device, f, &devInfo);
            if((devInfo.VendorID == 0xffff) | (devInfo.DeviceID == 0xffff))
                continue;

            idx++;

            if(registry_createdirectory("HW/PCI", idx_str) != registry_err_ok)
                return -1;


            if(registry_addkey_uint(key_idx, "BUS", bus) != registry_err_ok)
                return -2;

            if(registry_addkey_uint(key_idx, "DEVICE", device) != registry_err_ok)
                return -3;

            if(registry_addkey_uint(key_idx, "FUNCTION", f) != registry_err_ok)
                return -4;

            if(registry_addkey_uint(key_idx, "CLASS", devInfo.ClassCode) != registry_err_ok)
                return -5;

            if(registry_addkey_uint(key_idx, "SUBCLASS", devInfo.SubClassCode) != registry_err_ok)
                return -6;

            if(registry_addkey_uint(key_idx, "INTERFACE", devInfo.ProgIF) != registry_err_ok)
                return -7;

            if(registry_addkey_uint(key_idx, "DEVICE_ID", devInfo.DeviceID) != registry_err_ok)
                return -8;

            if(registry_addkey_uint(key_idx, "VENDOR_ID", devInfo.VendorID) != registry_err_ok)
                return -9;

            if(registry_addkey_uint(key_idx, "BAR_COUNT", devInfo.BarCount) != registry_err_ok)
                return -10;

            for(uint32_t mcfg_idx = 0; mcfg_idx < len / sizeof(MCFG_Entry); mcfg_idx++)
                if(mcfg->entries[mcfg_idx].start_bus_number <= bus && mcfg->entries[mcfg_idx].end_bus_number >= bus) {
                    uint64_t ecam_addr = mcfg->entries[mcfg_idx].baseAddr + ( (bus - mcfg->entries[mcfg_idx].start_bus_number) << 20 | device << 15 | f << 12 );
                    if(registry_addkey_uint(key_idx, "ECAM_ADDR", ecam_addr) != registry_err_ok)
                        return -20;

                    break;
                }
        }

        device++;
    }


    if(registry_addkey_uint("HW/PCI", "COUNT", idx) != registry_err_ok)
        return -14;

    return 0;
}