/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdlib.h>
#include <cardinal/local_spinlock.h>

#include "SysMP/mp.h"
#include "SysVirtualMemory/vmem.h"
#include "SysFP/fp.h"
#include "SysUser/syscall.h"
#include "terminal_priv.h"

#define KSTACK_SIZE (KiB(8))
#define PRIMARY_STACK_SIZE (KiB(32))

static terminaldef_t terminals;
static _Atomic terminal_tid = 0;
static _Atomic terminal_uid = 0;

typedef struct {
    int coreIdx;
    terminaldef_t *curTerm;
} terminal_state_t;

static _Atomic volatile int coreId = 0;
static TLS terminal_state_t *cur_state = NULL;
static terminaldef_t **queue = NULL;
static _Atomic volatile int *queue_lock = NULL;

void terminal_switch(terminaldef_t *nTerm) {

}

terminaldef_t* terminal_create(terminalflags_t flag, uint32_t uid, vmem_t *user_vmem, uint64_t ip) {
    terminaldef_t *tdef = malloc(sizeof(terminaldef_t));

    tdef->uid = uid;
    tdef->tid = terminal_tid++;
    tdef->map = user_vmem;    
    tdef->state = terminalstate_pending;
    tdef->flags = flag;
    tdef->u_stack = (flag & terminalflag_kernel) ? NULL : stack_alloc(PRIMARY_STACK_SIZE, true);
    tdef->k_stack = (flag & terminalflag_kernel) ? stack_alloc(PRIMARY_STACK_SIZE, false) : stack_alloc(KSTACK_SIZE, true);
    tdef->float_state = malloc(fp_platform_getstatesize());
    memset(tdef->float_state, 0, fp_platform_getstatesize());

    //TODO: make this be initialized based on the supplied parameters
    tdef->reg_state = malloc(syscall_getfullstate_size());
    memset(tdef->reg_state, 0, syscall_getfullstate_size());

    tdef->next = NULL;

    local_spinlock_lock(queue_lock[cur_state->coreIdx]);

    if(queue[cur_state->coreIdx] != NULL)
        queue[cur_state->coreIdx]->next = tdef;

    tdef->prev = queue[cur_state->coreIdx];
    queue[cur_state->CoreIdx] = tdef;

    local_spinlock_unlock(queue_lock[cur_state->coreIdx]);
}

void terminal_delete(terminaldef_t *term) {
    local_spinlock_lock(queue_lock[cur_state->coreIdx]);
    
    //TODO: Free the term state

    local_spinlock_unlock(queue_lock[cur_state->coreIdx]);
} 

int module_init(){
    queue = malloc(sizeof(terminal_state_t*) * mp_corecount());
    memset(queue, 0, sizeof(terminal_state_t*) * mp_corecount());

    queue_lock = malloc(sizeof(_Atomic volatile int) * mp_corecount());
    memset(queue_lock, 0, sizeof(_Atomic volatile int) * mp_corecount());

    if(cur_state == NULL)
        cur_state = (TLS terminal_state_t*)mp_tls_get(mp_tls_alloc(sizeof(terminal_state_t)));

    return 0;
}

int mp_init() {
    cur_state->coreIdx = coreId++; 

    //Wait and attempt to obtain a task
    //If tasks aren't obtained, we should hlt the cpu after setting the apic timer
}