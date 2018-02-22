/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <types.h>
#include "priv_timers.h"
#include "timer.h"
#include "SysInterrupts/interrupts.h"


#define IA32_TSC_DEADLINE 0x6e0

int local_apic_timer_init(bool tsc_mode, void (*handler)(int));
PRIVATE uint64_t tsc_read(timer_handlers_t *handlers);


PRIVATE void apic_handler(int irq) {

    interrupt_sendeoi(irq);
}

PRIVATE int apic_timer_init(){
    local_apic_timer_init(false, apic_handler);
    
    {
        timer_handlers_t main_counter;
        timer_features_t main_features = timer_features_persistent | timer_features_periodic;

        main_counter.rate = 2000;   //0.5ms
        main_counter.read = NULL;
        main_counter.write = NULL;
        main_counter.set_mode = NULL;       //TODO
        main_counter.set_enable = NULL;     //TODO
        main_counter.set_handler = NULL;    //TODO
        main_counter.send_eoi = NULL;

        timer_register(main_features, &main_counter);
    }
    
    return 0;
}

PRIVATE int apic_timer_tsc_init() {
    local_apic_timer_init(true, apic_handler);

    {
        timer_handlers_t main_counter;
        timer_features_t main_features = timer_features_read | timer_features_64bit | timer_features_persistent | timer_features_oneshot | timer_features_fixed_intr | timer_features_local;

        main_features |= timer_features_counter | timer_features_write;

        main_counter.rate = 0;  //TODO
        main_counter.read = NULL;
        main_counter.write = NULL;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;
        main_counter.send_eoi = NULL;

        timer_register(main_features, &main_counter);
    }
    return 0;
}