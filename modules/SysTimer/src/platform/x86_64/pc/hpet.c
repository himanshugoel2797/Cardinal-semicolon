/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

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
} PACKED HPET_TimerConfigRegister;

typedef struct {
    uint32_t Value;
    uint32_t Address;
} PACKED HPET_FSBRoute;

typedef struct {
    HPET_TimerConfigRegister Configuration;
    uint64_t ComparatorValue;
    HPET_FSBRoute InterruptRoute;
    uint64_t Rsv0;
} PACKED HPET_Timer;

typedef struct {
    uint8_t RevID;
    uint8_t TimerCount : 5;
    uint8_t Is64Bit : 1;
    uint8_t Rsv0 : 1;
    uint8_t LegacyReplacementCapable : 1;
    uint16_t VendorID;
    uint32_t ClockPeriod;
} PACKED HPET_CapRegister;

typedef struct {
    uint64_t GlobalEnable : 1;
    uint64_t LegacyReplacementEnable : 1;
    uint64_t Rsv0 : 62;
} PACKED HPET_ConfigRegister;

typedef struct {
    uint64_t T0_InterruptStatus : 1;
    uint64_t T1_InterruptStatus : 1;
    uint64_t T2_InterruptStatus : 1;
    uint64_t Rsv0 : 61;
} PACKED HPET_InterruptStatus;

typedef struct {
    HPET_CapRegister Capabilities;
    uint64_t Rsv0;
    HPET_ConfigRegister Configuration;
    uint64_t Rsv1;
    HPET_InterruptStatus InterruptStatus;
    uint64_t Rsv2;
    volatile uint64_t CounterValue;
    uint64_t Rsv3;
    HPET_Timer timers[0];
} PACKED HPET_Main;

typedef struct {
    HPET_Main *hpet;
    int timer_num;
    int intrpt_num;
} HPET_TimerState;

PRIVATE uint64_t hpet_main_read(timer_handlers_t *handler) {
    HPET_Main* hpet = (HPET_Main*)handler->state;

    return hpet->CounterValue;
}

PRIVATE uint64_t hpet_main_write(timer_handlers_t *handler, uint64_t val) {
    HPET_Main* hpet = (HPET_Main*)handler->state;

    hpet->CounterValue = val;
}



PRIVATE uint64_t hpet_timer_read(timer_handlers_t *handler) {
    HPET_Timer* timer = (HPET_Timer*)handler->state;
    return timer->ComparatorValue;
}

PRIVATE uint64_t hpet_timer_write(timer_handlers_t *handler, uint64_t val) {
    HPET_Timer* timer = (HPET_Timer*)handler->state;
    
    //If periodic, set appropriate comparator value bit
    if(timer->Configuration.TimerType)
        timer->Configuration.ValueSet = 1;
    timer->ComparatorValue = val;

    return 0;
}

PRIVATE uint64_t hpet_timer_setmode(timer_handlers_t *handler, timer_features_t features) {
    HPET_Timer* timer = (HPET_Timer*)handler->state;
    
    if(features & timer_features_oneshot) {
        timer->Configuration.TimerType = 0;
    }else if(features & timer_features_periodic) {
        timer->Configuration.TimerType = 1;
    }

    return 0;
}

PRIVATE uint64_t hpet_timer_setenable(timer_handlers_t *handler, bool enable) {
    HPET_Timer* timer = (HPET_Timer*)handler->state;
    timer->Configuration.InterruptEnable = enable;
    return 0;
}

PRIVATE uint64_t hpet_timer_sethandler(timer_handlers_t *state, void (*handler)()) {
    HPET_Timer* timer = (HPET_timer*)state->state;
    //TODO
    return 0;
}

PRIVATE uint64_t hpet_timer_sendeoi(timer_handlers_t *handler) {
    HPET_Timer* timer = (HPET_timer*)handler->state;
    //TODO
    return 0;
}

PRIVATE int hpet_getcount() {
    HPET_Main *base_addr = NULL;
    intptr_t hpet_phys_base_addr = 0;
    if(registry_readkey_uint("HW/HPET", "ADDRESS", (uint64_t*)&hpet_phys_base_addr) != registry_err_ok)
        return -1;

    base_addr = (HPET_Main*)vmem_phystovirt(hpet_phys_base_addr, sizeof(HPET_Main), vmem_flags_uncached | vmem_flags_kernel);

    return base_addr->Capabilities.TimerCount + 2;
}

PRIVATE int hpet_init(){

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

        main_features |= timer_features_counter | timer_features_write | timer_features_read;

        main_counter.rate = base_addr->Capabilities.ClockPeriod;
        main_counter.state = (uint64_t)base_addr;
        main_counter.read = hpet_main_read;
        main_counter.write = hpet_main_write;
        main_counter.set_mode = NULL;
        main_counter.set_enable = NULL;
        main_counter.set_handler = NULL;
        main_counter.send_eoi = NULL;

        timer_register(main_features, &main_counter);
    }

    //Enable MSI/FSB interrupt mode for timers, but keep interrupts disabled
    for(int i = 0; i < base_addr->Capabilities.TimerCount; i++){
        if(base_addr->timers[i].Configuration.FSBInterruptDelivery)
            PANIC("HPET does not support MSI!\r\n");

        base_addr->timers[i].Configuration.FSBInterruptEnable = 1;

        //Register remaining counters
        {
            timer_handlers_t sub_counter;
            timer_features_t sub_features = common_features;

            sub_features |= timer_features_write | timer_features_oneshot | timer_features_pcie_msg_intr;
            if(base_addr->timers[i].Configuration.PeriodicCapable)
                sub_features |= timer_features_periodic;

            sub_counter.rate = base_addr->Capabilities.ClockPeriod;
            sub_counter.state = &base_addr->timers[i];
            sub_counter.read = hpet_timer_read;
            sub_counter.write = hpet_timer_write;
            sub_counter.set_mode = hpet_timer_setmode;
            sub_counter.set_enable = hpet_timer_setenable;
            sub_counter.set_handler = hpet_timer_sethandler;
            sub_counter.send_eoi = hpet_timer_sendeoi;

            timer_register(sub_features, &sub_counter);
        }
    }

    //Enable the counter
    base_addr->Configuration.GlobalEnable = 1;

    return 0;
}