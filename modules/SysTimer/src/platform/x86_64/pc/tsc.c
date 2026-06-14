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

// Calibrate the TSC frequency (Hz) by counting cycles across a PIT-timed
// reference interval. Needed when CPUID leaf 0x15 doesn't report the rate
// (SysReg/cpuid.c then leaves HW/PROC/TSC_FREQ == 0, e.g. under QEMU/KVM). The
// PIT is driven through pit_oneshot_wait_ns() -- pit.c owns the hardware -- so
// the timer is not bit-banged from here.
static uint64_t tsc_calibrate_pit(void) {
    const uint64_t calib_ms = 10;

    uint64_t s_edx = 0, s_eax = 0;
    __asm__ volatile("rdtsc" : "=d"(s_edx), "=a"(s_eax));
    uint64_t start = (s_edx << 32) | (s_eax & 0xffffffff);

    pit_oneshot_wait_ns(calib_ms * 1000000ULL);

    uint64_t e_edx = 0, e_eax = 0;
    __asm__ volatile("rdtsc" : "=d"(e_edx), "=a"(e_eax));
    uint64_t end = (e_edx << 32) | (e_eax & 0xffffffff);

    // (cycles elapsed in calib_ms) scaled up to cycles per second.
    return (end - start) * (1000 / calib_ms);
}

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

        // CPUID leaf 0x15 often reports nothing under QEMU/KVM, leaving TSC_FREQ
        // at 0; calibrate against the PIT in that case so the TSC counter has a
        // real rate (a 0 rate breaks timer_timestamp_ns and any rate-based
        // busy-wait -- see timer_busywait_ns).
        uint64_t tsc_freq = 0;
        if(registry_readkey_uint("HW/PROC", "TSC_FREQ", &tsc_freq) != registry_err_ok)
            tsc_freq = 0;
        if(tsc_freq == 0)
            tsc_freq = tsc_calibrate_pit();
        if(tsc_freq == 0)
            return -1;  // no usable TSC rate; leave it to the APIC/HPET path
        main_counter.rate = tsc_freq;

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