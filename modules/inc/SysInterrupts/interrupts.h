// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_INTERRUPTS_H
#define CARDINAL_INTERRUPTS_H

#include <stdint.h>
#include <stdbool.h>
#include <cardinal/cs_error.h>

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

typedef struct interrupt_register_state interrupt_register_state_t;

#if defined(__x86_64__)
struct interrupt_register_state {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    uint64_t rflags;
    uint64_t rip;

    uint64_t cs;
    uint64_t ss;

    uint64_t rbp;
    uint64_t rsp;
};
#endif


void interrupt_sendeoi(int irq);

void interrupt_register_handler(int irq, InterruptHandler handler);

void interrupt_unregister_handler(int irq, InterruptHandler handler);

// Install a hook consulted on an UNHANDLED CPU exception (vector < 32), just
// before the trap frame is dumped and the kernel panics. SysTest uses this so a
// CPU fault triggered by a death test reports the fault vector and reboots
// instead of panicking into the debug shell. The hook is passed the vector and
// returns normally when it chooses not to act (e.g. no death test armed); pass
// NULL to clear. Resolved by name from SysTest.
void interrupt_set_death_hook(void (*hook)(int vector));

cs_error interrupt_allocate(int cnt, interrupt_flags_t flags, int *base);

void interrupt_mapinterrupt(uint32_t line, int irq, bool active_low, bool level_trig);

int interrupt_get_cpu_idx(void);

void interrupt_setmask(uint32_t line, bool mask);

void interrupt_sendipi(int cpu, int vector, ipi_delivery_mode_t delivery_mode);

void interrupt_setstack(void *stack);

void interrupt_set_register_state(const interrupt_register_state_t *state);

void interrupt_get_register_state(interrupt_register_state_t *state);

uint32_t interrupt_msi_register_addr(int cpu_idx);

uint64_t interrupt_msi_register_data(int vec);

#endif