/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <types.h>

#include "elf.h"

#define GDT_ENTRY_COUNT 8

typedef struct PACKED {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;

    uint8_t access;
    uint8_t granularity;

    uint8_t base_high;
} gdt_t;

typedef struct PACKED {
    uint16_t limit;
    gdt_t *base;
} gdtr_t;

typedef volatile struct tss_struct {
    uint32_t rsv0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t rsv1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t rsv2;
    uint16_t rsv3;
    uint16_t iomap;
} PACKED tss_struct_t;

typedef struct {
    gdt_t *gdt;
    tss_struct_t *tss;
} tls_gdt_t;

static TLS tls_gdt_t *gdt = NULL;

static void gdt_setentry(gdt_t *gdt, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt->base_low = (base & 0xFFFF);
    gdt->base_mid = (base >> 16) & 0xFF;
    gdt->base_high = (base >> 24) & 0xFF;
    gdt->limit_low = (limit & 0xFFFF);
    gdt->granularity = (limit >> 16) & 0x0F;
    gdt->granularity |= gran & 0xF0;
    gdt->access = access;
}

void gdt_settss(gdt_t* gdt_lcl, int num, uint64_t base, uint32_t limit, uint8_t access) {
    gdt_setentry(&gdt_lcl[num], (uint32_t)base, limit, access, 0);
    gdt_setentry(&gdt_lcl[num + 1], 0, 0, 0, 0);

    gdt_lcl[num + 1].limit_low = (uint16_t)(base >> 32);
    gdt_lcl[num + 1].base_low = (uint16_t)(base >> (32 + 16));
}

void interrupt_setstack(void *stack) {
    gdt->tss->ist1 = (uint64_t)stack;
}

int gdt_init() {

    if(gdt == NULL) {
        int (*mp_tls_alloc)(int) = elf_resolvefunction("mp_tls_alloc");
        TLS void* (*mp_tls_get)(int) = elf_resolvefunction("mp_tls_get");

        gdt = (TLS tls_gdt_t*)mp_tls_get(mp_tls_alloc(sizeof(tls_gdt_t)));
    }
    gdt->gdt = malloc(GDT_ENTRY_COUNT * sizeof(gdt_t));
    gdt->tss = malloc(sizeof(tss_struct_t));
    gdt->tss->ist1 = (uint64_t)malloc(4096) + 4096;     //Allocate temporary interrupt stack

    gdt_t* gdt_lcl = gdt->gdt;
    gdt_setentry(&gdt_lcl[0], 0, 0, 0, 0);
    gdt_setentry(&gdt_lcl[1], 0, 0xFFFFFFFF, 0x9B, 0xA0); // Code segment
    gdt_setentry(&gdt_lcl[2], 0, 0xFFFFFFFF, 0x93, 0x00); // Data segment
    gdt_setentry(&gdt_lcl[3], 0, 0xFFFFFFFF, 0xFB, 0xD0); // User mode code segment (32bit)
    gdt_setentry(&gdt_lcl[4], 0, 0xFFFFFFFF, 0xF3, 0x00); // User mode data segment
    gdt_setentry(&gdt_lcl[5], 0, 0xFFFFFFFF, 0xFB, 0xA0); // User mode code segment (64bit)
    gdt_settss(gdt_lcl, 6, (uint64_t)gdt->tss, sizeof(tss_struct_t), 0xE9);

    gdtr_t gdtr;
    gdtr.limit = (sizeof(gdt_t) * GDT_ENTRY_COUNT) - 1;
    gdtr.base = gdt_lcl;

    __asm__ volatile ("lgdt (%0)" :: "r" (&gdtr));

    __asm__ volatile (
        "pushq %rax\n\t"
        "mov $flush_gdt_btstrp, %rax\n\t"
        "pushq %rax\n\t"
        "retq\n\t"
        "flush_gdt_btstrp:\n\t"
        "mov $0x10, %ax\n\t"
        "mov %ax, %ds\n\t"
        "mov %ax, %es\n\t"
        "mov %ax, %fs\n\t"
        "mov %ax, %ss\n\t"

        //Set the TSS
        "mov $0x33, %ax\n\t"
        "ltr %ax\n\t"
        "popq %rax\n\t"
    );

    return 0;
}