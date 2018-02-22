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
PRIVATE int rtc_init();
PRIVATE int apic_timer_init();
PRIVATE int apic_timer_tsc_init();

PRIVATE bool use_tsc();
PRIVATE int tsc_init();

#endif