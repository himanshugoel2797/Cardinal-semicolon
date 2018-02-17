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
        XSDT *xsdt = (XSDT*)vmem_phystovirt_ptr((intptr_t)rsdp->XsdtAddress, MiB(2), vmem_flags_cachewriteback);
        if (!ACPITables_ValidateChecksum((ACPISDTHeader*)xsdt)) return (void*)-1;

        int entries = XSDT_GET_POINTER_COUNT((xsdt->h));
        int cur_index = 0;

        for (int i = 0; i < entries; i++) {
            if(xsdt->PointerToOtherSDT[i] == 0)continue;
            ACPISDTHeader *h = (ACPISDTHeader *)vmem_phystovirt_ptr((intptr_t)xsdt->PointerToOtherSDT[i], MiB(2), vmem_flags_cachewriteback);
            if (!strncmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h)) {
                if (cur_index == index)
                    return (void *) h;

                cur_index++;
            }
        }
    } else if ((rsdp->firstPart.Revision == ACPI_VERSION_1) | (!rsdp->XsdtAddress)) {
        RSDT *rsdt = (RSDT*)vmem_phystovirt_ptr((intptr_t)rsdp->firstPart.RsdtAddress, MiB(2), vmem_flags_cachewriteback);
        if (!ACPITables_ValidateChecksum((ACPISDTHeader*)rsdt)) return NULL;

        int entries = RSDT_GET_POINTER_COUNT((rsdt->h));
        int cur_index = 0;

        for (int i = 0; i < entries; i++) {
            ACPISDTHeader *h = (ACPISDTHeader*)vmem_phystovirt_ptr((intptr_t)rsdt->PointerToOtherSDT[i], MiB(2), vmem_flags_cachewriteback);
            if (!strncmp(h->Signature, table_name, 4) && ACPITables_ValidateChecksum(h)) {
                if (cur_index == index)
                    return (void *) h;

                cur_index++;
            }
        }
    }

    return NULL;
}

static int save_lapic(uint32_t idx, MADT_EntryLAPIC *lapic) {
    char idx_str[10] = "";
    char key_str[256] = "HW/LAPIC/";
    char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

    if(registry_createdirectory("HW/LAPIC", idx_str) != registry_err_ok)
        return -26;

    if(registry_addkey_uint(key_idx, "PROCESSOR ID", lapic->processor_id) != registry_err_ok)
        return -27;

    if(registry_addkey_uint(key_idx, "APIC ID", lapic->apic_id) != registry_err_ok)
        return -28;

    return 0;
}

static int save_ioapic(uint32_t idx, MADT_EntryIOAPIC *ioapic) {
    char idx_str[10] = "";
    char key_str[256] = "HW/IOAPIC/";
    char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

    if(registry_createdirectory("HW/IOAPIC", idx_str) != registry_err_ok)
        return -22;

    if(registry_addkey_uint(key_idx, "ID", ioapic->io_apic_id) != registry_err_ok)
        return -23;

    if(registry_addkey_uint(key_idx, "BASE_ADDR", ioapic->io_apic_base_addr) != registry_err_ok)
        return -24;

    if(registry_addkey_uint(key_idx, "GLOBAL_INTR_BASE", ioapic->global_sys_int_base) != registry_err_ok)
        return -25;

    return 0;
}

static int save_isaovr(uint32_t ioapic_cnt, MADT_EntryISAOVR *isaovr) {
    //Find the appropriate IOAPIC entry and add it to its overrides
    uint64_t prev_closest_idx = 0;
    {
        for(uint32_t i = 0; i < ioapic_cnt; i++) {
            char idx_str[10] = "";
            char key_str[256] = "HW/IOAPIC/";
            char *key_idx = strncat(key_str, itoa(i, idx_str, 16), 255);

            uint64_t intr_base = 0;
            if(registry_readkey_uint(key_idx, "GLOBAL_INTR_BASE", &intr_base) != registry_err_ok)
                return -15;

            if(isaovr->global_sys_int >= intr_base && (isaovr->global_sys_int - intr_base) <= prev_closest_idx)
                prev_closest_idx = i;
        }
    }

    char idx_str[10] = "";
    char idx2_str[10] = "";
    char key_str[256] = "HW/IOAPIC/";
    char *key_idx = strncat(key_str, itoa(prev_closest_idx, idx_str, 16), 255);

    int err = registry_createdirectory(key_idx, "OVERRIDE");
    if(err != registry_err_ok && err != registry_err_exists)
        return -16;

    key_idx = strncat(key_str, "/OVERRIDE", 255);
    itoa(isaovr->global_sys_int, idx2_str, 16);


    if(registry_createdirectory(key_idx, idx2_str) != registry_err_ok)
        return -17;

    key_idx = strncat(key_str, "/", 255);
    key_idx = strncat(key_str, idx2_str, 255);

    if(registry_addkey_uint(key_idx, "IRQ", isaovr->irq_src) != registry_err_ok)
        return -18;

    if(registry_addkey_uint(key_idx, "BUS", isaovr->bus_src) != registry_err_ok)
        return -19;

    if(registry_addkey_bool(key_idx, "ACTIVE_LOW", isaovr->flags & 2) != registry_err_ok)
        return -20;

    if(registry_addkey_bool(key_idx, "LEVEL_TRIGGER", isaovr->flags & 8) != registry_err_ok)
        return -21;

    return 0;
}

int preinit_acpi() {
    intptr_t rsdp_addr = 0;
    registry_readkey_uint("HW/BOOTINFO", "RSDPADDR", (uint64_t*)&rsdp_addr);
    RSDPDescriptor20* l_rsdp = (RSDPDescriptor20*)rsdp_addr;
    
    //Copy the rsdp
    rsdp = malloc(sizeof(RSDPDescriptor20));
    memcpy(rsdp, l_rsdp, sizeof(RSDPDescriptor20));
    return 0;
}

int acpi_init() {
    vmem_phystovirt_ptr = elf_resolvefunction("vmem_phystovirt");

    if(registry_createdirectory("HW", "ACPI") != registry_err_ok)
        return -1;

    {
        if(registry_createdirectory("HW", "LAPIC") != registry_err_ok)
            return -2;

        if(registry_createdirectory("HW", "IOAPIC") != registry_err_ok)
            return -3;

        MADT* madt = ACPITables_FindTable(MADT_SIG, 0);
        if(madt == NULL)
            return -4;

        uint32_t len = madt->h.Length - 8 - sizeof(ACPISDTHeader);
        uint32_t lapic_cnt = 0;
        uint32_t ioapic_cnt = 0;

        //Apply LAPICs and IOAPICs
        for(uint32_t i = 0; i < len; ) {
            MADT_EntryHeader *hdr = (MADT_EntryHeader*)&madt->entries[i];

            switch(hdr->type) {
            case MADT_LAPIC_ENTRY_TYPE: {
                int err = save_lapic(lapic_cnt++, (MADT_EntryLAPIC*)hdr);
                if(err != 0)
                    return err;
            }
            break;
            case MADT_IOAPIC_ENTRY_TYPE: {
                int err = save_ioapic(ioapic_cnt++, (MADT_EntryIOAPIC*)hdr);
                if(err != 0)
                    return err;
            }
            break;
            }

            i += hdr->entry_size;
            if(hdr->entry_size == 0) i += 8;
        }

        //Apply ISA overrides
        for(uint32_t i = 0; i < len; ) {
            MADT_EntryHeader *hdr = (MADT_EntryHeader*)&madt->entries[i];

            if(hdr->type == MADT_ISAOVER_ENTRY_TYPE) {
                int err = save_isaovr(ioapic_cnt, (MADT_EntryISAOVR*)hdr);
                if(err != 0)
                    return err;
            }
            
            i += hdr->entry_size;
            if(hdr->entry_size == 0) i += 8;
        }

        if(registry_addkey_uint("HW/IOAPIC", "COUNT", ioapic_cnt) != registry_err_ok)
            return -5;


        if(registry_addkey_uint("HW/LAPIC", "COUNT", lapic_cnt) != registry_err_ok)
            return -6;
    }

    {
        FADT* fadt = ACPITables_FindTable(FADT_SIG, 0);

    }

    {
        HPET* hpet = ACPITables_FindTable(HPET_SIG, 0);

        if(hpet != NULL){
            if(registry_createdirectory("HW", "HPET") != registry_err_ok)
                return -7;

            if(registry_addkey_uint("HW/HPET", "REVISION", hpet->RevisionID) != registry_err_ok)
                return -8;

            if(registry_addkey_uint("HW/HPET", "COMPARATOR_COUNT", hpet->ComparatorCount) != registry_err_ok)
                return -9;

            if(registry_addkey_bool("HW/HPET", "COUNTER_64BIT", hpet->CounterIs64Bit) != registry_err_ok)
                return -10;

            if(registry_addkey_bool("HW/HPET", "LEGACY_REPLACEMENT", hpet->LegacyReplacement) != registry_err_ok)
                return -11;

            if(registry_addkey_uint("HW/HPET", "VENDOR", hpet->VendorID) != registry_err_ok)
                return -12;

            if(registry_addkey_uint("HW/HPET", "ADDRESS", hpet->Address.address) != registry_err_ok)
                return -13;

            if(registry_addkey_uint("HW/HPET", "MINIMUM_TICK", hpet->MinimumTick) != registry_err_ok)
                return -14;
        }
    }

    {
        //TODO: This info goes into the PCI table
        MCFG* mcfg = ACPITables_FindTable(MCFG_SIG, 0);
    }

    return 0;
}