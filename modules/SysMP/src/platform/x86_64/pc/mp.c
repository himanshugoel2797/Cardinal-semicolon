/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "platform_mp.h"
#include "SysMP/mp.h"
#include "SysReg/registry.h"
#include "SysInterrupts/interrupts.h"
#include "elf.h"
#include <types.h>
#include <stdlib.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#define TLS_SIZE ((int)KiB(4))
#define GS_BASE_MSR (0xC0000101)
#define KERNEL_GS_BASE_MSR (0xC0000102)

static int mp_loc = 0;
static _Atomic volatile int pos = 0;
static TLS uint64_t* g_tls;
static _Atomic int coreCount = 1;
static volatile int core_ready = 0;

void alloc_ap_stack(void);

int mp_init() {

    void (*timer_wait)(uint64_t) = elf_resolvefunction("timer_wait");

    //Start initializing the APs
    uint64_t ap_cnt = 0;
    if(registry_readkey_uint("HW/LAPIC", "COUNT", &ap_cnt) != registry_err_ok)
        return -1;

    for(uint32_t i = 0; i < ap_cnt; i++) {
        char idx_str[10] = "";
        char key_str[256] = "HW/LAPIC/";
        char *key_idx = strncat(key_str, itoa(i, idx_str, 16), 255);

        uint64_t apic_id = 0;

        if(registry_readkey_uint(key_idx, "APIC ID", &apic_id) != registry_err_ok)
            return -1;

        if((int)apic_id != interrupt_get_cpuidx()){
            
            core_ready = 0;

            alloc_ap_stack();
            interrupt_sendipi(apic_id, 0x0, ipi_delivery_mode_init);

            //Use the timer api to wait for 10ms
            timer_wait(10 * 1000 * 1000);

            interrupt_sendipi(apic_id, 0x0f, ipi_delivery_mode_startup);

            while(!core_ready);
        }
    }

    return 0;
}

int mp_corecount(void) {
    return coreCount;
}

int mp_signalready() {

    coreCount++;
    core_ready = 1;

    while(true)
    ;

    return 0;
}

int mp_tls_setup() {
    uint64_t tls = (uint64_t)malloc(TLS_SIZE);
    if(tls == 0)
        return -1;

    g_tls = NULL;   //Set the g_tls internal offset to 0, so it refers to GS_BASE_MSR+0
    wrmsr(GS_BASE_MSR, tls);
    return 0;
}

int mp_tls_alloc(int bytes) {
    local_spinlock_lock(&mp_loc);
    if(pos + bytes >= TLS_SIZE) {
        local_spinlock_unlock(&mp_loc);
        return -1;
    }
    int p = pos;
    pos += bytes;
    local_spinlock_unlock(&mp_loc);

    return p;
}

TLS void* mp_tls_get(int off) {
    if(off >= TLS_SIZE)return NULL;
    return (TLS void*)(uintptr_t)off;
}

int mp_platform_getstatesize(void)
{
    return (0x88);
}

void mp_platform_getstate(void* buf)
{
    if(buf == NULL)
        PANIC("Parameter is null.");

    __asm__ volatile (
        "movq %rax, +0x0(%rdi)\n\t"
        "movq %rbx, +0x8(%rdi)\n\t"
        "movq %rcx, +0x10(%rdi)\n\t"
        "movq %rdx, +0x18(%rdi)\n\t"
        "movq %rsi, +0x20(%rdi)\n\t"
        "movq %rdi, +0x28(%rdi)\n\t"
        "movq %rbp, +0x30(%rdi)\n\t"
        "movq %rsp, +0x38(%rdi)\n\t"
        "movq %r8, +0x40(%rdi)\n\t"
        "movq %r9, +0x48(%rdi)\n\t"
        "movq %r10, +0x50(%rdi)\n\t"
        "movq %r11, +0x58(%rdi)\n\t"
        "movq %r12, +0x60(%rdi)\n\t"
        "movq %r13, +0x68(%rdi)\n\t"
        "movq %r14, +0x70(%rdi)\n\t"
        "movq %r15, +0x78(%rdi)\n\t"
        "pushf\n\t"
        "popq %rax\n\t"
        "movq %rax, +0x80(%rdi)\n\t"
        "movq (%rdi), %rax\n\t"
    );
}

void mp_platform_setstate(void* buf)
{
    if(buf == NULL)
        PANIC("Parameter is null.");

    __asm__ volatile (
        "movq +0x80(%rdi), %rax\n\t"
        "push %rax\n\t"
        "popf\n\t"
        "movq +0x0(%rdi), %rax\n\t"
        "movq +0x8(%rdi), %rbx\n\t"
        "movq +0x10(%rdi), %rcx\n\t"
        "movq +0x18(%rdi), %rdx\n\t"
        "movq +0x20(%rdi), %rsi\n\t"
        "movq +0x30(%rdi), %rbp\n\t"
        "movq +0x38(%rdi), %rsp\n\t"
        "movq +0x40(%rdi), %r8\n\t"
        "movq +0x48(%rdi), %r9\n\t"
        "movq +0x50(%rdi), %r10\n\t"
        "movq +0x58(%rdi), %r11\n\t"
        "movq +0x60(%rdi), %r12\n\t"
        "movq +0x68(%rdi), %r13\n\t"
        "movq +0x70(%rdi), %r14\n\t"
        "movq +0x78(%rdi), %r15\n\t"

        "movq +0x28(%rdi), %rdi\n\t"
    );
}