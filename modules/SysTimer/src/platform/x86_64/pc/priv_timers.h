// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSTIMER_PLAT_PRIV_H
#define CARDINAL_SYSTIMER_PLAT_PRIV_H

#include <types.h>
#include <stdbool.h>

PRIVATE int hpet_getcount();

PRIVATE int hpet_init();
PRIVATE int pit_init();
// Busy-wait `ns` nanoseconds using PIT channel 2 (the single owner of the PIT).
// Pure polling -- safe with interrupts disabled. Used as a hardware-timed
// reference for calibrating the TSC.
PRIVATE void pit_oneshot_wait_ns(uint64_t ns);
PRIVATE int rtc_init();
PRIVATE int apic_timer_init();
PRIVATE int apic_timer_tsc_init();

PRIVATE bool use_tsc();
PRIVATE int tsc_init();
PRIVATE int tsc_mp_init();

#endif