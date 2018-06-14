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
#include "SysInterrupts/interrupts.h"

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

    uint32_t high = ioapic_read(idx, high_index);
    high &= ~0xff000000;
    //high |= (0xff000000);
    //bsp is the destination
    high |= (interrupt_get_cpuidx() << 24);
    ioapic_write(idx, high_index, high);

    uint32_t low = ioapic_read(idx, low_index);

    // set the polarity
    low &= ~(1<<13);
    low |= ((active_low & 1) << 13);

    low &= ~(1<<15);
    low |= ((level_trigger & 1) << 15);

    // set delivery vector
    low &= ~0xff;
    low |= irq & 0xff;

    // set to fixed destination mode
    low &= ~(1<<11);
    low |= (0 << 11);

    // set to fixed delivery mode
    low &= ~0x700;
    low |= 0 << 8;


    // unmask the interrupt
    low &= ~(1<<16);

    ioapic_write(idx, low_index, low);
}

static void ioapic_setmask(uint32_t idx, uint32_t irq_pin, bool mask) {
    //configure this override
    const uint32_t low_index = 0x10 + irq_pin*2;
    uint32_t low = ioapic_read(idx, low_index);

    // unmask the interrupt
    low &= ~(1<<16);
    if(mask)
        low |= (1 << 16);

    ioapic_write(idx, low_index, low);
}


void interrupt_mapinterrupt(uint32_t line, int irq, bool active_low, bool level_trig) {
    int ioapic_idx = 0;
    uint32_t ioapic_close_intr_base = 0;

    for(int i = 0; i < ioapic_cnt; i++)
        if(ioapics[i].global_intr_base < line && ioapics[i].global_intr_base > ioapic_close_intr_base) {
            ioapic_close_intr_base = ioapics[i].global_intr_base;
            ioapic_idx = i;
        }

    ioapic_map(ioapic_idx, line, irq, active_low, level_trig);
}

void interrupt_setmask(uint32_t line, bool mask) {
    int ioapic_idx = 0;
    uint32_t ioapic_close_intr_base = 0;

    for(int i = 0; i < ioapic_cnt; i++)
        if(ioapics[i].global_intr_base < line && ioapics[i].global_intr_base > ioapic_close_intr_base) {
            ioapic_close_intr_base = ioapics[i].global_intr_base;
            ioapic_idx = i;
        }

    ioapic_setmask(ioapic_idx, line, mask);
}

int ioapic_init() {
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
            if(err == registry_err_dne) {
                //Configure this entry as normal
                ioapic_map(i, j, j + intr_base + 0x20, false, false);
                ioapic_setmask(i, j, true);
                continue;
            }

            if(err != registry_err_ok)
                return -1;

            registry_readkey_uint(key2_idx, "BUS", &bus);
            registry_readkey_bool(key2_idx, "ACTIVE_LOW", &active_low);
            registry_readkey_bool(key2_idx, "LEVEL_TRIGGER", &level_trigger);

            char int_buf[10];
            DEBUG_PRINT(itoa(irq, int_buf, 10));
            DEBUG_PRINT(":");
            DEBUG_PRINT(itoa(j + intr_base, int_buf, 10));
            if(active_low)
                DEBUG_PRINT(":active_low");
            if(level_trigger)
                DEBUG_PRINT(":level_trigger");
            DEBUG_PRINT("\r\n");

            ioapic_map(i, j, irq + 0x20, active_low, level_trigger);
            ioapic_setmask(i, j, true);

            int irq_num = irq + 0x20;
            //if(interrupt_allocate(1, interrupt_flags_exclusive | interrupt_flags_fixed, &irq_num) != 0)
            //    PANIC("Failed to reserve specified interrupt.");
        }
    }


    return 0;
}