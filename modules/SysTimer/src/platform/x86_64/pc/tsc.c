/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <string.h>

#include "SysReg/registry.h"
#include "priv_timers.h"
#include "timer.h"

PRIVATE bool use_tsc() {
    bool tsc_valid = false;
    if(registry_readkey_bool("HW/PROC", "TSC_AVAIL", &tsc_valid) != registry_err_ok)
        return false;

    bool tsc_deadline = false;
    if(registry_readkey_bool("HW/PROC", "TSC_DEADLINE", &tsc_deadline) != registry_err_ok)
        return false;

    bool tsc_invar = false;
    if(registry_readkey_bool("HW/PROC", "TSC_INVARIANT", &tsc_invar) != registry_err_ok)
        return false;

    uint64_t tsc_freq = 0;
    if(registry_readkey_uint("HW/PROC", "TSC_FREQ", &tsc_freq) != registry_err_ok)
        return false;

    uint64_t apic_freq = 0;
    if(registry_readkey_uint("HW/PROC", "APIC_FREQ", &apic_freq) != registry_err_ok)
        return false;
    
    return true;
    //return (tsc_valid && tsc_deadline && tsc_invar && tsc_freq != 0 && apic_freq != 0);
}

PRIVATE uint64_t tsc_read(timer_handlers_t *handlers) {
    handlers = NULL;

    uint64_t edx = 0, eax = 0;
    __asm__ volatile("rdtsc" : "=d"(edx), "=a"(eax));
    return (edx << 32) | (eax & 0xffffffff);
}

PRIVATE int tsc_init() {
    //Setup the tsc
    uint64_t cr4 = 0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4) :: );
    cr4 |= (1 << 2);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    //Add the tsc as a counter
    {
        timer_handlers_t main_counter = { .name = "tsc" };
        timer_features_t main_features = timer_features_persistent | timer_features_counter | timer_features_read;

        if(registry_readkey_uint("HW/PROC", "TSC_FREQ", &main_counter.rate) != registry_err_ok)
            return -1;

        //strncpy(main_counter.name, "tsc", 16);
        main_counter.read = tsc_read;
        main_counter.write = NULL;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;

        timer_register(main_features, &main_counter);
    }

    //Initialize the apic timer
    return apic_timer_init();   //TODO: May want to use the TSC deadline mode
}

PRIVATE int tsc_mp_init() {
    //Setup the tsc
    uint64_t cr4 = 0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4) :: );
    cr4 |= (1 << 2);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    //Initialize the apic timer
    return apic_timer_init();   //TODO: May want to use the TSC deadline mode
}