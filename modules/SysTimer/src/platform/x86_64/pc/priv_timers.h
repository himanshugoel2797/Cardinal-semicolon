#ifndef CARDINAL_SYSTIMER_PLAT_PRIV_H
#define CARDINAL_SYSTIMER_PLAT_PRIV_H

#include <types.h>

PRIVATE int hpet_getcount();

PRIVATE int hpet_init();
PRIVATE int pit_init();
PRIVATE int rtc_init();
PRIVATE int apic_timer_init();

#endif