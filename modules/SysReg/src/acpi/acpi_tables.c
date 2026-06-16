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
#include "SysVirtualMemory/vmem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

#include "elf.h"

static intptr_t (*vmem_phystovirt_ptr)(intptr_t, size_t, int);
static RSDPDescriptor20 *rsdp;

bool ACPITables_ValidateChecksum(ACPISDTHeader *header)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < header->Length; i++)
    {
        sum += ((char *)header)[i];
    }

    return sum == 0;
}

void *ACPITables_FindTable(const char *table_name)
{
    if (rsdp == NULL)
        return NULL;

    if (rsdp->firstPart.Revision != ACPI_VERSION_1 && rsdp->XsdtAddress)
    {
        XSDT *xsdt = (XSDT *)vmem_phystovirt_ptr((intptr_t)rsdp->XsdtAddress, MiB(2), vmem_flags_cachewriteback);
        if (!ACPITables_ValidateChecksum((ACPISDTHeader *)xsdt))
            return (void *)-1;

        int entries = XSDT_GET_POINTER_COUNT((xsdt->h));

        for (int i = 0; i < entries; i++)
        {
            if (xsdt->PointerToOtherSDT[i] == 0)
                continue;
            ACPISDTHeader *h = (ACPISDTHeader *)vmem_phystovirt_ptr((intptr_t)xsdt->PointerToOtherSDT[i], MiB(2), vmem_flags_cachewriteback);
            char tmp_table_name[5];
            tmp_table_name[4] = 0;
            tmp_table_name[0] = h->Signature[0];
            tmp_table_name[1] = h->Signature[1];
            tmp_table_name[2] = h->Signature[2];
            tmp_table_name[3] = h->Signature[3];
            if (!memcmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h))
            {
                return (void *)h;
            }
        }
    }
    else if ((rsdp->firstPart.Revision == ACPI_VERSION_1) | (!rsdp->XsdtAddress))
    {
        RSDT *rsdt = (RSDT *)vmem_phystovirt_ptr((intptr_t)rsdp->firstPart.RsdtAddress, MiB(2), vmem_flags_cachewriteback);
        if (!ACPITables_ValidateChecksum((ACPISDTHeader *)rsdt))
            return NULL;

        int entries = RSDT_GET_POINTER_COUNT((rsdt->h));

        for (int i = 0; i < entries; i++)
        {
            ACPISDTHeader *h = (ACPISDTHeader *)vmem_phystovirt_ptr((intptr_t)rsdt->PointerToOtherSDT[i], MiB(2), vmem_flags_cachewriteback);
            if (!memcmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h))
            {
                return (void *)h;
            }
        }
    }

    return NULL;
}

static int save_lapic(uint32_t idx, MADT_EntryLAPIC *lapic)
{
    char idx_str[10] = "";
    char key_str[256] = "HW/LAPIC/";
    char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

    if (registry_createdirectory("HW/LAPIC", idx_str) != CS_OK)
        return -26;

    if (registry_addkey_uint(key_idx, "PROCESSOR ID", lapic->processor_id) != CS_OK)
        return -27;

    if (registry_addkey_uint(key_idx, "APIC ID", lapic->apic_id) != CS_OK)
        return -28;

    return 0;
}

static int save_ioapic(uint32_t idx, MADT_EntryIOAPIC *ioapic)
{
    char idx_str[10] = "";
    char key_str[256] = "HW/IOAPIC/";
    char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

    if (registry_createdirectory("HW/IOAPIC", idx_str) != CS_OK)
        return -22;

    if (registry_addkey_uint(key_idx, "ID", ioapic->io_apic_id) != CS_OK)
        return -23;

    if (registry_addkey_uint(key_idx, "BASE_ADDR", ioapic->io_apic_base_addr) != CS_OK)
        return -24;

    if (registry_addkey_uint(key_idx, "GLOBAL_INTR_BASE", ioapic->global_sys_int_base) != CS_OK)
        return -25;

    return 0;
}

static int save_isaovr(uint32_t ioapic_cnt, MADT_EntryISAOVR *isaovr)
{
    //Find the appropriate IOAPIC entry and add it to its overrides
    uint64_t prev_closest_idx = 0;
    {
        for (uint32_t i = 0; i < ioapic_cnt; i++)
        {
            char idx_str[10] = "";
            char key_str[256] = "HW/IOAPIC/";
            char *key_idx = strncat(key_str, itoa(i, idx_str, 16), 255);

            uint64_t intr_base = 0;
            if (registry_readkey_uint(key_idx, "GLOBAL_INTR_BASE", &intr_base) != CS_OK)
                return -15;

            if (isaovr->global_sys_int >= intr_base && (isaovr->global_sys_int - intr_base) <= prev_closest_idx)
                prev_closest_idx = i;
        }
    }

    char idx_str[10] = "";
    char idx2_str[10] = "";
    char key_str[256] = "HW/IOAPIC/";
    char *key_idx = strncat(key_str, itoa(prev_closest_idx, idx_str, 16), 255);

    int err = registry_createdirectory(key_idx, "OVERRIDE");
    if (err != CS_OK && err != CS_EXISTS)
        return -16;

    key_idx = strncat(key_str, "/OVERRIDE", 255);
    itoa(isaovr->global_sys_int, idx2_str, 16);

    if (registry_createdirectory(key_idx, idx2_str) != CS_OK)
        return -17;

    key_idx = strncat(key_str, "/", 255);
    key_idx = strncat(key_str, idx2_str, 255);

    if (registry_addkey_uint(key_idx, "IRQ", isaovr->irq_src) != CS_OK)
        return -18;

    if (registry_addkey_uint(key_idx, "BUS", isaovr->bus_src) != CS_OK)
        return -19;

    if (registry_addkey_bool(key_idx, "ACTIVE_LOW", isaovr->flags & 2) != CS_OK)
        return -20;

    if (registry_addkey_bool(key_idx, "LEVEL_TRIGGER", isaovr->flags & 8) != CS_OK)
        return -21;

    return 0;
}

int WEAK print_uint64(uint64_t v, uint8_t base);
int preinit_acpi()
{
    intptr_t rsdp_addr = 0;
    registry_readkey_uint("HW/BOOTINFO", "RSDPADDR", (uint64_t *)&rsdp_addr);
    RSDPDescriptor20 *l_rsdp = (RSDPDescriptor20 *)rsdp_addr;

    //Copy the rsdp
    rsdp = malloc(sizeof(RSDPDescriptor20));
    memcpy(rsdp, l_rsdp, sizeof(RSDPDescriptor20));

    DEBUG_PRINT("RSDP at: ");
    print_uint64((uint64_t)l_rsdp, 16);

    DEBUG_PRINT("XSDT at: ");
    print_uint64((uint64_t)l_rsdp->XsdtAddress, 16);

    print_hexdump((uint8_t*)l_rsdp, 128);
    print_hexdump((uint8_t*)rsdp, 128);

    return 0;
}

int acpi_init()
{
    vmem_phystovirt_ptr = elf_resolvefunction("vmem_phystovirt");

    if (registry_createdirectory("HW", "ACPI") != CS_OK)
        return -1;

    {
        if (registry_createdirectory("HW", "LAPIC") != CS_OK)
            return -2;

        if (registry_createdirectory("HW", "IOAPIC") != CS_OK)
            return -3;

        MADT *madt = ACPITables_FindTable(MADT_SIG);
        if (madt == NULL)
            return -4;

        uint32_t len = madt->h.Length - 8 - sizeof(ACPISDTHeader);
        uint32_t lapic_cnt = 0;
        uint32_t ioapic_cnt = 0;

        //Apply LAPICs and IOAPICs
        for (uint32_t i = 0; i < len;)
        {
            MADT_EntryHeader *hdr = (MADT_EntryHeader *)&madt->entries[i];

            switch (hdr->type)
            {
            case MADT_LAPIC_ENTRY_TYPE:
            {
                int err = save_lapic(lapic_cnt++, (MADT_EntryLAPIC *)hdr);
                if (err != 0)
                    return err;
            }
            break;
            case MADT_IOAPIC_ENTRY_TYPE:
            {
                int err = save_ioapic(ioapic_cnt++, (MADT_EntryIOAPIC *)hdr);
                if (err != 0)
                    return err;
            }
            break;
            }

            i += hdr->entry_size;
            if (hdr->entry_size == 0)
                i += 8;
        }

        //Apply ISA overrides
        for (uint32_t i = 0; i < len;)
        {
            MADT_EntryHeader *hdr = (MADT_EntryHeader *)&madt->entries[i];

            if (hdr->type == MADT_ISAOVER_ENTRY_TYPE)
            {
                int err = save_isaovr(ioapic_cnt, (MADT_EntryISAOVR *)hdr);
                if (err != 0)
                    return err;
            }

            i += hdr->entry_size;
            if (hdr->entry_size == 0)
                i += 8;
        }

        if (registry_addkey_uint("HW/IOAPIC", "COUNT", ioapic_cnt) != CS_OK)
            return -5;

        if (registry_addkey_uint("HW/LAPIC", "COUNT", lapic_cnt) != CS_OK)
            return -6;
    }

    {
        FADT *fadt = ACPITables_FindTable(FADT_SIG);

        if (registry_createdirectory("HW", "FADT") != CS_OK)
            return -7;

        if (fadt->h.Revision > 1) {
            if (registry_addkey_bool("HW/FADT", "8042", (fadt->BootArchitectureFlags & 2) >> 1) != CS_OK)
                return -8;
        } else {
            if (registry_addkey_bool("HW/FADT", "8042", true) != CS_OK)
                return -8;
        }
    }

    {
        HPET *hpet = ACPITables_FindTable(HPET_SIG);

        if (hpet != NULL)
        {
            if (registry_createdirectory("HW", "HPET") != CS_OK)
                return -9;

            if (registry_addkey_uint("HW/HPET", "REVISION", hpet->RevisionID) != CS_OK)
                return -10;

            if (registry_addkey_uint("HW/HPET", "COMPARATOR_COUNT", hpet->ComparatorCount) != CS_OK)
                return -11;

            if (registry_addkey_bool("HW/HPET", "COUNTER_64BIT", hpet->CounterIs64Bit) != CS_OK)
                return -12;

            if (registry_addkey_bool("HW/HPET", "LEGACY_REPLACEMENT", hpet->LegacyReplacement) != CS_OK)
                return -13;

            if (registry_addkey_uint("HW/HPET", "VENDOR", hpet->VendorID) != CS_OK)
                return -14;

            if (registry_addkey_uint("HW/HPET", "ADDRESS", hpet->Address.address) != CS_OK)
                return -15;

            if (registry_addkey_uint("HW/HPET", "MINIMUM_TICK", hpet->MinimumTick) != CS_OK)
                return -16;
        }
    }

    return 0;
}