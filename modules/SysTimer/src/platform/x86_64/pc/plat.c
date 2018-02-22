/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "priv_timers.h"

int timer_platform_gettimercount(){
    return hpet_getcount() + 2;
}

int timer_platform_init(){

    //If TSC timer is constant rate + consistent, just use APIC timer in TSC mode

        //Initialize HPET timer - use as reference
        if(hpet_init() != 0){
            //Initialize PIT timer - use as reference if HPET is not available
            pit_init();
        }
        //Initialize APIC timer - callibrated relative to reference clock
        apic_timer_init();

    //Initialize RTC timer - absolute timer/clock
    rtc_init();


    return 0;
}