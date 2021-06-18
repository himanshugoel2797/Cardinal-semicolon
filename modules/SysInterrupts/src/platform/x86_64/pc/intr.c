/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <types.h>
#include <stdlib.h>

#include "SysInterrupts/interrupts.h"

int idt_init();
int gdt_init();
int ioapic_init();
int apic_init();
int pic_fini();

static void spurious_irq_handler(int int_num)
{
    int_num = 0;
}

static void pagefault_handler(int int_num)
{
    int_num = 0;

    interrupt_register_state_t reg_state;
    interrupt_getregisterstate(&reg_state);

    char tmp[20];
    DEBUG_PRINT("Page fault at: ");
    DEBUG_PRINT(ltoa(reg_state.rip, tmp, 16));
    DEBUG_PRINT("\r\n");

    PANIC("");
}

int intr_init()
{
    int err = 0;

    pic_fini();

    err = gdt_init();
    if (err != 0)
        return err;

    err = idt_init();
    if (err != 0)
        return err;

    int irq0 = 0x27;
    int irq1 = 0x2f;
    int pf_intr = 0x0e;
    interrupt_allocate(1, interrupt_flags_exclusive, &irq0);
    interrupt_allocate(1, interrupt_flags_exclusive, &irq1);
    interrupt_allocate(1, interrupt_flags_exclusive, &pf_intr);

    interrupt_registerhandler(irq0, spurious_irq_handler);
    interrupt_registerhandler(irq1, spurious_irq_handler);
    interrupt_registerhandler(pf_intr, pagefault_handler);

    err = ioapic_init();
    if (err != 0)
        return err;

    err = apic_init();
    if (err != 0)
        return err;

    //__asm__("hlt");
    __asm__("sti");

    return 0;
}

int intr_mp_init()
{
    int err = 0;

    err = gdt_init();
    if (err != 0)
        return err;

    err = idt_init();
    if (err != 0)
        return err;

    err = apic_init();
    if (err != 0)
        return err;

    //__asm__("hlt");
    //__asm__("sti");

    return 0;
}

uint32_t msi_register_addr(int cpu_idx)
{
    return 0xFEE00000 | (cpu_idx & 0xff) << 12 | (1 << 3) | (0 << 2); //fixed destination mode
}

uint64_t msi_register_data(int vec)
{
    return (vec & 0xff);
}