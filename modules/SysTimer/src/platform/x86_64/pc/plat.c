/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "priv_timers.h"
#include "SysInterrupts/interrupts.h"

int timer_platform_gettimercount(){
    return hpet_getcount() + (use_tsc() ? 2 : 0) + 2;
}

int timer_platform_init(){
    //Disable the PIT
    outb(0x43, 0x30);
    outb(0x40, 0x00);
    outb(0x40, 0x00);

    //If TSC timer is constant rate + consistent and the rate is known, just use APIC timer in TSC mode
    if(use_tsc()) {
        tsc_init();
    }else{
        //Initialize HPET timer - use as reference
        if(hpet_init() != 0){
            //Initialize PIT timer - use as reference if HPET is not available
            pit_init();
        }
        //Initialize APIC timer as a periodic timer
        apic_timer_init();
    }

    //Initialize RTC timer - absolute timer/clock
    rtc_init();

    return 0;
}

int timer_mp_init(){
    if(use_tsc()) {
        tsc_mp_init();
    } else{
        apic_timer_init();
    }

    return 0;
}