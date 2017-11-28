/**
 * Copyright (c) 2017 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <types.h>

int idt_init();
int gdt_init();
int ioapic_init();
int apic_init();

int intr_init(){
    int err = 0;
    
    err = gdt_init();
    if(err != 0)
        return err;

    err = idt_init();
    if(err != 0)
        return err;

    err = ioapic_init();
    if(err != 0)
        return err;

    err = apic_init();
    if(err != 0)
        return err;

    __asm__("sti");

    return 0;
}