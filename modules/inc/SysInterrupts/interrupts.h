// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_INTERRUPTS_H
#define CARDINAL_INTERRUPTS_H

#include <stdint.h>

typedef void (*InterruptHandler)(int);

typedef enum {
    interrupt_flags_none = 0,
    interrupt_flags_exclusive = (1 << 0),
    interrupt_flags_fixed = (1 << 1),
} interrupt_flags_t;

typedef enum {
    ipi_delivery_mode_fixed = 0,
    ipi_delivery_mode_init = 5,
    ipi_delivery_mode_startup = 6,
} ipi_delivery_mode_t;

void interrupt_sendeoi(int irq);

void interrupt_registerhandler(int irq, InterruptHandler handler);

void interrupt_unregisterhandler(int irq, InterruptHandler handler);

int interrupt_allocate(int cnt, interrupt_flags_t flags, int *base);

void interrupt_mapinterrupt(uint32_t line, int irq, bool active_low, bool level_trig);

int interrupt_get_cpuidx(void);

void interrupt_setmask(uint32_t line, bool mask);

void interrupt_sendipi(int cpu, int vector, ipi_delivery_mode_t delivery_mode);

void interrupt_setstack(void *stack);

uint32_t msi_register_addr(int cpu_idx);

uint64_t msi_register_data(int vec);

#endif