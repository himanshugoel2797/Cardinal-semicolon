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

// Forward decl: the recursive scan and the per-function recorder call each
// other (a bridge function recurses into its secondary bus).
static int PCI_ScanBus(MCFG *mcfg, uint32_t len, uint32_t *idx, uint32_t bus);

// Record one PCI function in the registry, then -- if it is a PCI-to-PCI bridge
// -- follow its secondary bus so devices behind it are enumerated too. Returns
// 0 (incl. the "absent function" no-op), or a negative registry error.
static int PCI_RecordFunction(MCFG *mcfg, uint32_t len, uint32_t *idx,
                              uint32_t bus, uint32_t device, uint32_t f) {
    PCI_Device devInfo;
    PCI_GetPCIDevice(bus, device, f, &devInfo);
    if((devInfo.VendorID == 0xffff) | (devInfo.DeviceID == 0xffff))
        return 0;

    char idx_str[10] = "";
    char key_str[256] = "HW/PCI/";
    char *key_idx = strncat(key_str, itoa(*idx, idx_str, 16), 255);

    (*idx)++;

    if(registry_createdirectory("HW/PCI", idx_str) != CS_OK)
        return -1;

    if(registry_addkey_uint(key_idx, "BUS", bus) != CS_OK)
        return -2;

    if(registry_addkey_uint(key_idx, "DEVICE", device) != CS_OK)
        return -3;

    if(registry_addkey_uint(key_idx, "FUNCTION", f) != CS_OK)
        return -4;

    if(registry_addkey_uint(key_idx, "CLASS", devInfo.ClassCode) != CS_OK)
        return -5;

    if(registry_addkey_uint(key_idx, "SUBCLASS", devInfo.SubClassCode) != CS_OK)
        return -6;

    if(registry_addkey_uint(key_idx, "INTERFACE", devInfo.ProgIF) != CS_OK)
        return -7;

    if(registry_addkey_uint(key_idx, "DEVICE_ID", devInfo.DeviceID) != CS_OK)
        return -8;

    if(registry_addkey_uint(key_idx, "VENDOR_ID", devInfo.VendorID) != CS_OK)
        return -9;

    if(registry_addkey_uint(key_idx, "BAR_COUNT", devInfo.BarCount) != CS_OK)
        return -10;

    for(uint32_t mcfg_idx = 0; mcfg_idx < len / sizeof(MCFG_Entry); mcfg_idx++)
        if(mcfg->entries[mcfg_idx].start_bus_number <= bus && mcfg->entries[mcfg_idx].end_bus_number >= bus) {
            uint64_t ecam_addr = mcfg->entries[mcfg_idx].baseAddr + ( (bus - mcfg->entries[mcfg_idx].start_bus_number) << 20 | device << 15 | f << 12 );
            if(registry_addkey_uint(key_idx, "ECAM_ADDR", ecam_addr) != CS_OK)
                return -20;

            break;
        }

    // PCI-to-PCI bridge (header type 1, MF bit masked off): descend into the
    // bus it forwards to. Secondary-bus number lives in byte 1 of the dword at
    // config offset 0x18. Only follow strictly-downstream buses (secondary >
    // bus), which holds for any sane bus-number assignment and rules out cycles.
    if((devInfo.HdrType & 0x7f) == 1) {
        uint32_t secondary = (PCI_ReadDWord(bus, device, f, 0x18) >> 8) & 0xff;
        if(secondary > bus) {
            int r = PCI_ScanBus(mcfg, len, idx, secondary);
            if(r != 0)
                return r;
        }
    }

    return 0;
}

// Scan every device/function on one bus, recursing through any bridges found.
static int PCI_ScanBus(MCFG *mcfg, uint32_t len, uint32_t *idx, uint32_t bus) {
    for(uint32_t device = 0; device < 32; device++) {
        if(PCI_ReadDWord(bus, device, 0, 0) == 0xFFFFFFFF)
            continue;

        uint32_t funcs = PCI_GetFuncCount(bus, device);
        for(uint32_t f = 0; f < funcs; f++) {
            int r = PCI_RecordFunction(mcfg, len, idx, bus, device, f);
            if(r != 0)
                return r;
        }
    }

    return 0;
}

int pci_reg_init() {

    uint32_t idx = 0;

    if(registry_createdirectory("HW", "PCI") != CS_OK)
        return -1;

    //Find the ECAM address if possible
    MCFG* mcfg = ACPITables_FindTable(MCFG_SIG);
    uint32_t len = mcfg->h.Length - 8 - sizeof(ACPISDTHeader);

    // Enumerate the whole tree starting at the root bus, following bridges.
    int r = PCI_ScanBus(mcfg, len, &idx, 0);
    if(r != 0)
        return r;

    if(registry_addkey_uint("HW/PCI", "COUNT", idx) != CS_OK)
        return -14;

    return 0;
}