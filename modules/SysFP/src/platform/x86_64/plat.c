/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <string.h>

#include "SysReg/registry.h"

#include "fpu.h"


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
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);   //Clear EM bit, enabling FPU
    cr0 |= (1 << 1);    //Set MP bit, to allow lazy state saving
    cr0 |= (1 << 5);    //Enable native FPU error interrupts
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    //Enable SSE and related through the XSAVE description
    uint64_t cr4 = 0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);    //OSFXSR
    cr4 |= (1 << 10);   //OSXMMEXCPT
    if(xsave)cr4 |= (1 << 18);   //OSXSAVE
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    __asm__ volatile ("fninit");
    if(xsave) {
        __asm__ volatile("xsetbv" :: "d"(xsave_bits >> 32), "a"(xsave_bits & 0xffffffff), "c"((uint32_t)0));
    }

    return 0;
}

int fp_platform_getstatesize(void) {
    if(xsave)
        return (xsave_sz + 64);
    return 512;
}

int fp_platform_getalign(void) {
    if(xsave)
        return 64;
    return 16;
}

void fp_platform_getstate(void* buf) {
    if(xsave)
        __asm__ volatile("xsaveq (%0)" :: "r"(buf), "d"(0xffffffffffffffff), "a"(0xffffffffffffffff) : "memory");
    else
        __asm__ volatile("fxsaveq (%0)" :: "r"(buf) : "memory");
}

void fp_platform_setstate(void* buf) {
    if(xsave)
        __asm__ volatile("xrstorq (%0)" :: "r"(buf), "d"(0xffffffffffffffff), "a"(0xffffffffffffffff) : "memory");
    else
        __asm__ volatile("fxrstorq (%0)" :: "r"(buf) : "memory");
}

void fp_platform_getdefaultstate(void* buf) {
    //FPU ctrl word = 0x33F
    //MXCSR = 0x1f80

    memset(buf, 0, fp_platform_getstatesize());
    
    uint16_t *buf_u16 = (uint16_t*)buf;
    buf_u16[0] = 0x33f;
    buf_u16[12] = 0x1f80;
}