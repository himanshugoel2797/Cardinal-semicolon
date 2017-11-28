/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <types.h>

#include "SysVirtualMemory/vmem.h"

#include "elf.h"

#define ICW4_8086 0x01    /* 8086/88 (MCS-80/85) mode */
#define ICW1_ICW4 0x01    /* ICW4 (not) needed */
#define ICW1_INIT 0x10 /* Initialization - required! */

#define APIC_ID (0x20 / sizeof(uint32_t))
#define APIC_SVR (0xF0 / sizeof(uint32_t))
#define APIC_TPR (0x80 / sizeof(uint32_t))
#define APIC_DFR (0xE0 / sizeof(uint32_t))

typedef struct {
    uint32_t *base_addr;
    uint32_t id;
} tls_apic_t;

static TLS tls_apic_t *apic = NULL;

uint32_t apic_read(uint32_t off) {
    return apic->base_addr[off];
}

void apic_write(uint32_t off, uint32_t val) {
    apic->base_addr[off] = val;
}

int apic_init(){

    //Disable the PIC
    outb(0x20, ICW1_INIT+ICW1_ICW4); // starts the initialization sequence (in cascade mode)
    outb(0xA0, ICW1_INIT+ICW1_ICW4);
    outb(0x21, 32);           // ICW2: Master PIC vector offset
    outb(0xA1, 40);           // ICW2: Slave PIC vector offset
    outb(0x21, 4);                 // ICW3: tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    outb(0xA1, 2);                 // ICW3: tell Slave PIC its cascade identity (0000 0010)

    outb(0x21, ICW4_8086);
    outb(0xA1, ICW4_8086);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF); //disable all interrupts from the PIC


    if(apic == NULL) {
        int (*mp_tls_alloc)(int) = elf_resolvefunction("mp_tls_alloc");
        TLS void* (*mp_tls_get)(int) = elf_resolvefunction("mp_tls_get");

        apic = (TLS tls_apic_t*)mp_tls_get(mp_tls_alloc(sizeof(tls_apic_t)));
    }

    uint64_t apic_base_reg = rdmsr(IA32_APIC_BASE);
    apic->base_addr = (uint32_t*)vmem_phystovirt(apic_base_reg & ~0xfff, KiB(8), vmem_flags_uncached);
    apic->id = apic_read(APIC_ID);
    apic_base_reg |= (1 << 11);
    wrmsr(IA32_APIC_BASE, apic_base_reg);

    apic_write(APIC_DFR, 0xf0000000);
    apic_write(APIC_TPR, 0);
    apic_write(APIC_SVR, (1 << 8) | 0xFF);

    return 0;
}

//configure timer
//task switches don't require callibration
//sleep operations do require callibration - given frequency timers