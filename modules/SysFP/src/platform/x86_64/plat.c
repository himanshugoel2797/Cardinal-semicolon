/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "fpu.h"

#include "SysReg/registry.h"

static bool xsave = false;
static uint64_t xsave_bits = 0;
static uint64_t xsave_sz = 0;
int fp_platform_init() {

    //check for xsave support
    registry_readkey_bool("HW/PROC", "XSAVE", &xsave);
    registry_readkey_uint("HW/PROC", "XSAVE_BITS", &xsave_bits);
    registry_readkey_uint("HW/PROC", "XSAVE_SZ", &xsave_sz);

    //Enable FPU
    uint64_t cr0 = 0;
    __asm__ volatile("mov %cr0, %%0" : "=r"(cr0));
    cr0 &= ~(1 << 2);   //Clear EM bit, enabling FPU
    cr0 |= (1 << 1);    //Set MP bit, to allow lazy state saving
    cr0 |= (1 << 3);    //Set TS bit, to cause interrupt on first FPU instruction
    cr0 |= (1 << 5);    //Enable native FPU error interrupts
    __asm__ volatile("mov %%0, %cr0" :: "r"(cr0));

    //Enable SSE and related through the XSAVE description
    uint64_t cr4 = 0;
    __asm__ volatile("mov %cr4, %%0" : "=r"(cr4));
    cr4 |= (1 << 9);    //OSFXSR
    cr4 |= (1 << 10);   //OSXMMEXCPT
    if(xsave)cr4 |= (1 << 18);   //OSXSAVE
    __asm__ volatile("mov %%0, %cr4" :: "r"(cr4));

    if(xsave) {
        __asm__ volatile("xsetbv" :: "d"(xsave_bits >> 32), "a"(xsave_bits & 0xffffffff), "c"((uint32_t)0));
    }

    return 0;
}

int fp_platform_getstatesize(void) {
    if(xsave)
        return xsave_sz;
    return 512;
}

void fp_platform_getstate(void* buf) {
    if(xsave)
        __asm__ volatile("xsaveq (%0)" :: "r"(buf) : "memory");
    else
        __asm__ volatile("fxsaveq (%0)" :: "r"(buf) : "memory");
}

void fp_platform_setstate(void* buf) {
    if(xsave)
        __asm__ volatile("xrstorq (%0)" :: "r"(buf) : "memory");
    else
        __asm__ volatile("fxrstorq (%0)" :: "r"(buf) : "memory");
}