/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "priv_timers.h"

#include "SysReg/registry.h"

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
    HPET_TimerConfigRegister Configuration;
    uint64_t ComparatorValue;
    uint64_t FSBInterruptRoute;
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
    HPET_Timer timer0;
    HPET_Timer timer1;
    HPET_Timer timer2;
} PACKED HPET_Main;


PRIVATE int hpet_init(){

    HPET_Main *base_addr = NULL;
    if(registry_readkey_uint("HW/HPET", "ADDRESS", (uint64_t*)&base_addr) != registry_err_ok)
        return -1;

    //Initialize the main counter
    base_addr->Configuration.GlobalEnable = 0;  //Disable the counter during setup
    base_addr->CounterValue = 0;

    //Fill out the timer configuration
    timer_features_t common_features = 0;

    if(base_addr->Capabilities.Is64Bit)
        common_features |= timer_features_64bit;

    //HPET supports MSI/FSB interrupts
    common_features |= timer_features_pcie_msg_intr;


    //Enable the counter
    base_addr->Configuration.GlobalEnable = 1;

    return 0;
}