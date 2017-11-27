/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "platform_mp.h"
#include "SysMP/mp.h"
#include <types.h>
#include <stdlib.h>
#include <cardinal/local_spinlock.h>

#define TLS_SIZE ((int)KiB(4))
#define GS_BASE_MSR (0xC0000101)
#define KERNEL_GS_BASE_MSR (0xC0000102)

static int mp_loc = 0;
static _Atomic volatile int pos = 0;
static TLS uint64_t* g_tls;

int mp_init() {

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