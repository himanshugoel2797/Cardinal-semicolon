/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>

#include "SysInterrupts/interrupts.h"

#include "priv_timers.h"
#include "timer.h"

#include "SysReg/registry.h"
#include "SysVirtualMemory/vmem.h"

typedef struct {
    uint64_t Rsv0 : 1;
    uint64_t InterruptType : 1;
    uint64_t InterruptEnable : 1;
    uint64_t TimerType : 1;
    uint64_t PeriodicCapable : 1;
    uint64_t Is64Bit : 1;
    uint64_t ValueSet : 1;
    uint64_t Rsv1 : 1;
    uint64_t Enable32BitMode : 1;
    uint64_t InterruptRoute : 5;
    uint64_t FSBInterruptEnable : 1;
    uint64_t FSBInterruptDelivery : 1;
    uint64_t Rsv2 : 16;
    uint64_t RoutingCapability : 32;
} HPET_TimerConfigRegister;

typedef struct {
    uint32_t Value;
    uint32_t Address;
} HPET_FSBRoute;

typedef struct {
    HPET_TimerConfigRegister Configuration;
    uint64_t ComparatorValue;
    HPET_FSBRoute InterruptRoute;
    uint64_t Rsv0;
} HPET_Timer;

typedef struct {
    uint16_t RevID : 8;
    uint16_t TimerCount : 5;
    uint16_t Is64Bit : 1;
    uint16_t Rsv0 : 1;
    uint16_t LegacyReplacementCapable : 1;
    uint16_t VendorID;
    uint32_t ClockPeriod;
} HPET_CapRegister;

typedef struct {
    uint64_t GlobalEnable : 1;
    uint64_t LegacyReplacementEnable : 1;
    uint64_t Rsv0 : 62;
} HPET_ConfigRegister;

typedef struct {
    HPET_CapRegister Capabilities;
    uint64_t Rsv0;
    HPET_ConfigRegister Configuration;
    uint64_t Rsv1;
    uint64_t InterruptStatus;
    uint64_t Rsv2[25];
    volatile uint64_t CounterValue;
    uint64_t Rsv3;
    HPET_Timer timers[0];
} HPET_Main;

typedef struct {
    HPET_Main *hpet;
    bool enabled;
    void (*cur_handler)(int);
} HPET_TimerState;

static HPET_TimerState *timers;

PRIVATE uint64_t hpet_main_read(timer_handlers_t *handler) {
    HPET_Main* hpet = (HPET_Main*)handler->state;

    return hpet->CounterValue;
}

PRIVATE void hpet_main_write(timer_handlers_t *handler, uint64_t val) {
    HPET_Main* hpet = (HPET_Main*)handler->state;

    hpet->CounterValue = val;
}



PRIVATE uint64_t hpet_timer_read(timer_handlers_t *handler) {
    HPET_TimerState *timer = &timers[handler->state];
    return timer->hpet->timers[handler->state].ComparatorValue;
}

PRIVATE void hpet_timer_write(timer_handlers_t *handler, uint64_t val) {
    HPET_TimerState *timer = &timers[handler->state];

    //If periodic, set appropriate comparator value bit
    if(timer->hpet->timers[handler->state].Configuration.TimerType)
        timer->hpet->timers[handler->state].Configuration.ValueSet = 1;

    timer->hpet->timers[handler->state].ComparatorValue = timer->hpet->CounterValue + val;

    if(timer->hpet->timers[handler->state].Configuration.TimerType)
        timer->hpet->timers[handler->state].ComparatorValue = val;
}

PRIVATE uint64_t hpet_timer_setmode(timer_handlers_t *handler, timer_features_t features) {
    HPET_TimerState *timer = &timers[handler->state];

    if(features & timer_features_oneshot) {
        timer->hpet->timers[handler->state].Configuration.TimerType = 0;
    } else if(features & timer_features_periodic) {
        timer->hpet->timers[handler->state].Configuration.TimerType = 1;
    }

    return 0;
}

PRIVATE void hpet_timer_setenable(timer_handlers_t *handler, bool enable) {
    HPET_TimerState *timer = &timers[handler->state];
    timer->enabled = enable;
    timer->hpet->timers[handler->state].Configuration.InterruptEnable = enable;
}

PRIVATE void hpet_timer_sethandler(timer_handlers_t *state, void (*handler)(int)) {
    HPET_TimerState *timer = &timers[state->state];
    timer->cur_handler = handler;
}

PRIVATE void hpet_timer_sendeoi(timer_handlers_t *handler) {
    HPET_TimerState *timer = &timers[handler->state];
    timer->hpet->InterruptStatus = (1 << handler->state);
}

PRIVATE void hpet_timer_handler(int irq) {
    HPET_Main *hpet = timers[0].hpet;
    for(int i = 0; i <= hpet->Capabilities.TimerCount; i++) {
        if(timers[i].enabled) {
            if(timers[i].cur_handler != NULL)
                timers[i].cur_handler(irq);
            hpet->InterruptStatus = (1u << i);
        }
    }
}

PRIVATE int hpet_getcount() {
    HPET_Main *base_addr = NULL;
    intptr_t hpet_phys_base_addr = 0;
    if(registry_readkey_uint("HW/HPET", "ADDRESS", (uint64_t*)&hpet_phys_base_addr) != registry_err_ok)
        return -1;

    base_addr = (HPET_Main*)vmem_phystovirt(hpet_phys_base_addr, sizeof(HPET_Main), vmem_flags_uncached | vmem_flags_kernel);

    return base_addr->Capabilities.TimerCount + 2;
}

PRIVATE int hpet_init() {

    HPET_Main *base_addr = NULL;
    intptr_t hpet_phys_base_addr = 0;
    if(registry_readkey_uint("HW/HPET", "ADDRESS", (uint64_t*)&hpet_phys_base_addr) != registry_err_ok)
        return -1;

    base_addr = (HPET_Main*)vmem_phystovirt(hpet_phys_base_addr, sizeof(HPET_Main), vmem_flags_uncached | vmem_flags_kernel);

    //Initialize the main counter
    base_addr->Configuration.GlobalEnable = 0;  //Disable the counter during setup
    base_addr->CounterValue = 0;

    //Fill out the timer configuration
    timer_features_t common_features = timer_features_persistent | timer_features_read;
    if(base_addr->Capabilities.Is64Bit)
        common_features |= timer_features_64bit;

    //Register main counter
    {
        timer_handlers_t main_counter;
        timer_features_t main_features = common_features;

        main_features |= timer_features_counter | timer_features_write;

        strncpy(main_counter.name, "hpet_main", 16);
        main_counter.rate = base_addr->Capabilities.ClockPeriod;
        main_counter.state = (uint64_t)base_addr;
        main_counter.read = hpet_main_read;
        main_counter.write = hpet_main_write;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;

        timer_register(main_features, &main_counter);
    }

    //Allocate interrupt handler
    int intrpt_num = 240;   //low priority interrupt
    interrupt_allocate(1, interrupt_flags_none, &intrpt_num);
    timers = (HPET_TimerState*)malloc(sizeof(HPET_TimerState) * base_addr->Capabilities.TimerCount + 1);
    interrupt_registerhandler(intrpt_num, hpet_timer_handler);

    //Enable MSI/FSB interrupt mode for timers, but keep interrupts disabled
    for(int i = 0; i <= base_addr->Capabilities.TimerCount; i++) {

        //Register remaining counters
        {
            timer_handlers_t sub_counter;
            timer_features_t sub_features = common_features;
            sub_features |= timer_features_write | timer_features_oneshot;

            HPET_TimerState *tState = &timers[i];
            tState->hpet = base_addr;

            //Configure MSI information
            if(base_addr->timers[i].Configuration.FSBInterruptDelivery) {
                sub_features |= timer_features_pcie_msg_intr;

                base_addr->timers[i].Configuration.FSBInterruptEnable = 1;
                base_addr->timers[i].InterruptRoute.Address = msi_register_addr(interrupt_get_cpuidx());
                base_addr->timers[i].InterruptRoute.Value = msi_register_data(intrpt_num);
            } else {
                sub_features |= timer_features_fixed_intr;

                //All hpet timers use the same interrupt vector
                //Try to configure all timers to use the same ioapic line
                //If not possible, map the next available line to the same vector
                int line = 0;
                int sup_lines = base_addr->timers[i].Configuration.RoutingCapability;
                while((sup_lines & 1) == 0) {
                    sup_lines = sup_lines >> 1;
                    line++;
                }

                base_addr->timers[i].Configuration.InterruptRoute = line;

                interrupt_mapinterrupt(line, intrpt_num, false, false);
                interrupt_setmask(line, false);
            }


            if(base_addr->timers[i].Configuration.PeriodicCapable)
                sub_features |= timer_features_periodic;

            strncpy(sub_counter.name, "hpet_sub", 16);
            sub_counter.name[8] = (i + '0');
            sub_counter.name[9] = 0;

            sub_counter.rate = base_addr->Capabilities.ClockPeriod;
            sub_counter.state = i;
            sub_counter.read = hpet_timer_read;
            sub_counter.write = hpet_timer_write;
            sub_counter.set_mode = hpet_timer_setmode;
            sub_counter.set_enable = hpet_timer_setenable;
            sub_counter.set_handler = hpet_timer_sethandler;

            timer_register(sub_features, &sub_counter);

            //Ensure that all timers are disabled
            hpet_timer_setenable(&sub_counter, false);
        }
    }

    //Enable the counter
    base_addr->Configuration.GlobalEnable = 1;

    return 0;
}