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
    return sizeof(interrupt_register_state_t);
}

void mp_platform_getstate(void* buf)
{
    if(buf == NULL)
        PANIC("Parameter is null.");

    interrupt_getregisterstate( (interrupt_register_state_t*) buf);
}

void mp_platform_setstate(void* buf)
{
    if(buf == NULL)
        PANIC("Parameter is null.");

    interrupt_setregisterstate( (interrupt_register_state_t*) buf);
}

void mp_platform_getdefaultstate(void *buf)
{
    memset(buf, 0, mp_platform_getstatesize());
}