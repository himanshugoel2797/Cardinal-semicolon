/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <types.h>
#include "priv_timers.h"
#include "timer.h"
#include "SysInterrupts/interrupts.h"
#include "SysMP/mp.h"
#include "SysReg/registry.h"

#define IA32_TSC_DEADLINE 0x6e0

typedef struct {
    bool tsc_mode;
    bool enabled;
    bool oneshot;
    void (*handler)(int);
} tls_apic_timer_state_t;

int local_apic_timer_init(bool tsc_mode, void (*handler)(int), bool ap);
PRIVATE uint64_t tsc_read(timer_handlers_t *handlers);
static TLS tls_apic_timer_state_t *apic_state = NULL;

PRIVATE void apic_handler(int irq) {
    if(apic_state->enabled) {
        //DEBUG_PRINT("APIC Triggered!");
        if(apic_state->handler != NULL)
            apic_state->handler(irq);

        if(apic_state->oneshot){
            apic_state->enabled = false;
        }
    }
    interrupt_sendeoi(irq);
}

PRIVATE uint64_t apic_timer_setmode(timer_handlers_t *handler, timer_features_t features) {
    handler = NULL;
    
    if(features & timer_features_oneshot) {
        apic_state->oneshot = true;
    }else if(features & timer_features_periodic) {
        apic_state->oneshot = false;
    }

    return 0;
}

PRIVATE void apic_timer_setenable(timer_handlers_t *handler, bool enable) {
    handler = NULL;
    apic_state->enabled = enable;
}

PRIVATE void apic_timer_sethandler(timer_handlers_t *state, void (*handler)(int)) {
    state = NULL;
    apic_state->handler = handler;
}


static bool apic_init_done = false;
PRIVATE int apic_timer_init(){
    local_apic_timer_init(false, apic_handler, apic_init_done);
    
    if(apic_state == NULL) {
        apic_state = (TLS tls_apic_timer_state_t*)mp_tls_get(mp_tls_alloc(sizeof(tls_apic_timer_state_t)));

        {
            timer_handlers_t main_counter;
            timer_features_t main_features = timer_features_persistent | timer_features_oneshot | timer_features_periodic | timer_features_local;

            strncpy(main_counter.name, "apic_local", 16);
            main_counter.rate = 20000;   //0.05ms
            main_counter.read = NULL;
            main_counter.write = NULL;
            main_counter.set_mode = apic_timer_setmode;       //TODO
            main_counter.set_enable = apic_timer_setenable;     //TODO
            main_counter.set_handler = apic_timer_sethandler;    //TODO

            timer_register(main_features, &main_counter);
        }
    }

    apic_state->tsc_mode = false;
    apic_state->enabled = false;
    apic_state->oneshot = false;
    apic_state->handler = NULL;
    
    apic_init_done = true;
    return 0;
}

PRIVATE int apic_timer_tsc_init() {
    local_apic_timer_init(true, apic_handler, apic_init_done);

    {
        timer_handlers_t main_counter;
        timer_features_t main_features = timer_features_read | timer_features_64bit | timer_features_persistent | timer_features_oneshot | timer_features_periodic | timer_features_fixed_intr | timer_features_local;

        main_features |= timer_features_counter | timer_features_write;

        if(registry_readkey_uint("HW/PROC", "TSC_FREQ", &main_counter.rate) != registry_err_ok)
            return -1;

        strncpy(main_counter.name, "apic_tsc", 16);
        main_counter.read = NULL;
        main_counter.write = NULL;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;

        timer_register(main_features, &main_counter);
    }
    return 0;
}