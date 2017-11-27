/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "acpi/acpi_tables.h"
#include "acpi/madt.h"
#include "acpi/fadt.h"
#include "acpi/hpet.h"
#include "acpi/mcfg.h"

#include "registry.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

#include "elf.h"

static intptr_t (*vmem_phystovirt)(intptr_t, size_t);
static RSDPDescriptor20 *rsdp;

static bool ACPITables_ValidateChecksum(ACPISDTHeader *header) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < header->Length; i++) {
        sum += ((char *)header)[i];
    }

    return sum == 0;
}

static void* ACPITables_FindTable(const char *table_name, int index) {
    if (rsdp == NULL) return NULL;


    if(rsdp->firstPart.Revision != ACPI_VERSION_1 && rsdp->XsdtAddress) {
        XSDT *xsdt = (XSDT*)vmem_phystovirt((intptr_t)rsdp->XsdtAddress, MiB(2));
        if (!ACPITables_ValidateChecksum((ACPISDTHeader*)xsdt)) return (void*)-1;

        int entries = XSDT_GET_POINTER_COUNT((xsdt->h));
        int cur_index = 0;

        for (int i = 0; i < entries; i++) {
            if(xsdt->PointerToOtherSDT[i] == 0)continue;
            ACPISDTHeader *h = (ACPISDTHeader *)vmem_phystovirt((intptr_t)xsdt->PointerToOtherSDT[i], MiB(2));
            if (!strncmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h)) {
                if (cur_index == index)
                    return (void *) h;

                cur_index++;
            }
        }
    } else if ((rsdp->firstPart.Revision == ACPI_VERSION_1) | (!rsdp->XsdtAddress)) {
        RSDT *rsdt = (RSDT*)vmem_phystovirt((intptr_t)rsdp->firstPart.RsdtAddress, MiB(2));
        if (!ACPITables_ValidateChecksum((ACPISDTHeader*)rsdt)) return NULL;

        int entries = RSDT_GET_POINTER_COUNT((rsdt->h));
        int cur_index = 0;

        for (int i = 0; i < entries; i++) {
            ACPISDTHeader *h = (ACPISDTHeader*)vmem_phystovirt((intptr_t)rsdt->PointerToOtherSDT[i], MiB(2));
            if (!strncmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h)) {
                if (cur_index == index)
                    return (void *) h;

                cur_index++;
            }
        }
    }

    return NULL;
}

int acpi_init() {

    vmem_phystovirt = elf_resolvefunction("vmem_phystovirt");

    intptr_t rsdp_addr = 0;
    registry_readkey_uint("HW/BOOTINFO", "RSDPADDR", (uint64_t*)&rsdp_addr);
    rsdp = (RSDPDescriptor20*)rsdp_addr;

    {
        MADT* madt = ACPITables_FindTable(MADT_SIG, 0);

    }

    {
        FADT* fadt = ACPITables_FindTable(FADT_SIG, 0);

    }

    {
        HPET* hpet = ACPITables_FindTable(HPET_SIG, 0);

    }

    {
        MCFG* mcfg = ACPITables_FindTable(MCFG_SIG, 0);

    }

    return 0;
}