/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "SysVirtualMemory/vmem.h"
#include "SysReg/registry.h"

typedef struct {
    uint32_t id;
    uint32_t global_intr_base;
    uint32_t volatile *base_addr;
} ioapic_t;

static ioapic_t *ioapics;
static int ioapic_cnt;

static void ioapic_write(int idx, uint32_t off, uint32_t val) {
    ioapics[idx].base_addr[0] = off;
    ioapics[idx].base_addr[4] = val;
}

static uint32_t ioapic_read(int idx, uint32_t off) {
    ioapics[idx].base_addr[0] = off;
    return ioapics[idx].base_addr[4];
}

static void ioapic_map(uint32_t idx, uint32_t irq_pin, uint32_t irq, bool active_low, bool level_trigger) {

            //configure this override
            const uint32_t low_index = 0x10 + irq_pin*2;
            const uint32_t high_index = 0x10 + irq_pin*2 + 1;

            //  TODO: learn how logical destinations work
            uint32_t high = ioapic_read(idx, high_index);
            // set APIC ID
            high &= ~0xff000000;
            high |= 1 << 24;
            ioapic_write(idx, high_index, high);
            

            uint32_t low = ioapic_read(idx, low_index);

            // set the polarity
            low &= ~(1<<13);
            low |= ((active_low & 1) << 13);

            low &= ~(1<<15);
            low |= ((level_trigger & 1) << 15);

            // set to physical delivery mode
            low &= ~(1<<11);

            // set delivery vector
            low &= ~0xff;
            low |= irq & 0xff;

            // set to fixed delivery mode
            low &= ~0x700;

            ioapic_write(idx, low_index, low);
}

int ioapic_init(){
    //Read the registry
    uint64_t count = 0;
    if(registry_readkey_uint("HW/IOAPIC", "COUNT", &count) != registry_err_ok)
        return -1;

    ioapics = malloc(sizeof(ioapic_t) * count);
    ioapic_cnt = (int)count;

    for(int i = 0; i < ioapic_cnt; i++) {
        char idx_str[10] = "";
        char key_str[256] = "HW/IOAPIC/";
        char *key_idx = strncat(key_str, itoa(i, idx_str, 16), 255);

        uint64_t id = 0;
        uint64_t base_addr = 0;
        uint64_t intr_base = 0;

        if(registry_readkey_uint(key_idx, "ID", &id) != registry_err_ok)
            return -1;

        if(registry_readkey_uint(key_idx, "BASE_ADDR", &base_addr) != registry_err_ok)
            return -1;

        if(registry_readkey_uint(key_idx, "GLOBAL_INTR_BASE", &intr_base) != registry_err_ok)
            return -1;

        ioapics[i].id = (uint32_t)id;
        ioapics[i].base_addr = (uint32_t volatile *)vmem_phystovirt(base_addr, KiB(8), vmem_flags_uncached);
        ioapics[i].global_intr_base = (uint32_t)intr_base;
    
        //Configure the detected overrides
        uint64_t available_redirs = ((ioapic_read(i, 0x01) >> 16) & 0xff) + 1;

        for(uint64_t j = 0; j < available_redirs; j++) {
            char idx2_str[10] = "";
            char key2_str[256] = "";

            char *key2_idx = strncpy(key2_str, key_idx, 255);
            strncat(key2_str, "/OVERRIDE/", 255);
            strncat(key2_str, itoa(j + intr_base, idx2_str, 16), 255);

            uint64_t irq = 0;
            uint64_t bus = 0;
            bool active_low = false;
            bool level_trigger = false;

            int err = registry_readkey_uint(key2_idx, "IRQ", &irq);
            if(err == registry_err_dne){
                //Configure this entry as normal
                ioapic_map(i, j, j + intr_base, false, false);
                continue;
            }

            if(err != registry_err_ok)
                return -1;

            registry_readkey_uint(key2_idx, "BUS", &bus);
            registry_readkey_bool(key2_idx, "ACTIVE_LOW", &active_low);
            registry_readkey_bool(key2_idx, "LEVEL_TRIGGER", &level_trigger);
            
            ioapic_map(i, j, irq, active_low, level_trigger);
        }
    }


    return 0;
}