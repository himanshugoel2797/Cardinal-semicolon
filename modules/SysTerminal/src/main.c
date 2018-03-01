/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cardinal/local_spinlock.h>

#include "SysMP/mp.h"
#include "SysVirtualMemory/vmem.h"
#include "SysFP/fp.h"
#include "SysUser/syscall.h"
#include "terminal_priv.h"

#define KSTACK_SIZE (KiB(8))
#define PRIMARY_STACK_SIZE (KiB(32))

static terminaldef_t terminals;
static _Atomic uint32_t terminal_tid = 0;
static _Atomic uint32_t terminal_uid = 0;

typedef struct {
    int coreIdx;
} terminal_state_t;

typedef struct {
    terminaldef_t *curTerm;
    terminaldef_t *queue;
    int pendingCnt;
    int queue_lock;
} queue_state_t;

static _Atomic volatile int coreId = 0;
static TLS terminal_state_t *cur_state = NULL;
static queue_state_t *qs = NULL;

bool terminal_steal(void) {
    //Iterate through cores, lock and take threads that are pending
    terminaldef_t *n_terms = NULL;
    terminaldef_t *end_terms = NULL;

    for(int i = 0; i < mp_corecount(); i++ ) {
        if(i == cur_state->coreIdx)
            continue;

        local_spinlock_lock(&qs[i].queue_lock);
        if(qs[i].pendingCnt) {
            terminaldef_t *prev = NULL;
            terminaldef_t *thrd = qs[i].queue;
            while(thrd != NULL) {
                terminaldef_t *n_thrd = NULL;
                local_spinlock_lock(&thrd->lock);
                n_thrd = thrd->next;
                if(thrd->state == terminalstate_pending)
                {
                    //append this terminal to the list to append to this cpu
                    if(prev != NULL)prev->next = thrd->next;
                    thrd->next = n_terms;
                    n_terms = thrd;
                    qs[i].pendingCnt--;

                    //If this is the first terminal taken, this is the tail of the list
                    if(end_terms == NULL)
                        end_terms = n_terms;

                    //Don't update prev in this case, because if the current item is removed
                    //the previous item remains the same
                }else
                    prev = thrd;

                local_spinlock_unlock(&thrd->lock);
                thrd = n_thrd;

                if(qs[i].pendingCnt == 0)
                    break;
            }

            local_spinlock_unlock(&qs[i].queue_lock);

            //Add the newly obtained threads to the queue of this cpu
            local_spinlock_lock(&qs[cur_state->coreIdx].queue_lock);
            end_terms->next = qs[cur_state->coreIdx].queue;
            qs[cur_state->coreIdx].queue = n_terms;
            local_spinlock_unlock(&qs[cur_state->coreIdx].queue_lock);
            return true;
        }
        local_spinlock_unlock(&qs[i].queue_lock);
    }
    return false;
}

//Make the next terminal the active one
void terminal_cycle(void) {
    local_spinlock_lock(&qs[cur_state->coreIdx].queue_lock);

    if(qs[cur_state->coreIdx].curTerm == NULL) {
        qs[cur_state->coreIdx].curTerm = qs[cur_state->coreIdx].queue;
    }else if(qs[cur_state->coreIdx].curTerm->next == NULL) {
        qs[cur_state->coreIdx].curTerm = qs[cur_state->coreIdx].queue;
        terminal_steal();   //Try stealing some terminals after restarting the queue
    }else {
        qs[cur_state->coreIdx].curTerm = qs[cur_state->coreIdx].curTerm->next;
    }

    //If still no terminal to run, unlock, enable cpu timer, hlt and try again
    while (qs[cur_state->coreIdx].curTerm == NULL) {

        //TODO: enable cpu timer and configure a blank handler to resume the cpu
        local_spinlock_unlock(&qs[cur_state->coreIdx].queue_lock);
        __asm__("hlt"); //TODO: abstract this assembly into a platform header
        local_spinlock_lock(&qs[cur_state->coreIdx].queue_lock);

        //If managed to steal some terminals, resume execution on this cpu
        if(terminal_steal()) {
            qs[cur_state->coreIdx].curTerm = qs[cur_state->coreIdx].queue;
            break;
        }
    }
    local_spinlock_unlock(&qs[cur_state->coreIdx].queue_lock);
}

terminaldef_t* terminal_create(terminalflags_t flag, uint32_t uid, vmem_t *user_vmem, uint64_t ip) {
    terminaldef_t *tdef = malloc(sizeof(terminaldef_t));

    tdef->uid = uid;
    tdef->tid = terminal_tid++;
    tdef->map = user_vmem;    
    tdef->state = terminalstate_pending;
    tdef->lock = 0;
    tdef->flags = flag;
    tdef->u_stack = (flag & terminalflag_kernel) ? NULL : stack_alloc(PRIMARY_STACK_SIZE, true);
    tdef->k_stack = (flag & terminalflag_kernel) ? stack_alloc(PRIMARY_STACK_SIZE, false) : stack_alloc(KSTACK_SIZE, true);
    tdef->float_state = malloc(fp_platform_getstatesize());
    memset(tdef->float_state, 0, fp_platform_getstatesize());

    //TODO: make this be initialized based on the supplied parameters
    tdef->reg_state = malloc(syscall_getfullstate_size());
    memset(tdef->reg_state, 0, syscall_getfullstate_size());

    tdef->next = NULL;

    local_spinlock_lock(&qs[cur_state->coreIdx].queue_lock);

    qs[cur_state->coreIdx].pendingCnt++;

    tdef->next = qs[cur_state->coreIdx].queue;
    qs[cur_state->coreIdx].queue = tdef;


    local_spinlock_unlock(&qs[cur_state->coreIdx].queue_lock);

    return tdef;
}

int terminal_setstate(uint32_t tid, terminalstate_t state) {
    for(int i = 0; i < mp_corecount(); i++ ) {
        //Iterate through cores, search for thread
        //Set the state using some rules

        //state == pending : (any)
        //state == running : blocked exiting
        //state == blocked : running exiting
        //state == exiting: (none)

        local_spinlock_lock(&qs[i].queue_lock);
        terminaldef_t *thrd = qs[i].queue;
        while(thrd != NULL) {
            
            local_spinlock_lock(&thrd->lock);
            if(thrd->tid == tid){
                int err = 0;
                switch(thrd->state) {
                    case terminalstate_pending:
                        thrd->state = state;
                    break;
                    case terminalstate_running:
                        if(state == terminalstate_blocked)
                            thrd->state = terminalstate_blocked;
                        else if(state == terminalstate_exiting)
                            thrd->state = terminalstate_exiting;
                        else
                            err = -1;
                    break;
                    case terminalstate_blocked:
                        if(state == terminalstate_running)
                            thrd->state = terminalstate_running;
                        else if(state == terminalstate_exiting)
                            thrd->state = terminalstate_exiting;
                        else 
                            err = -1;
                    break;
                    case terminalstate_exiting:
                        if(state == terminalstate_exiting)
                            thrd->state = terminalstate_exiting;
                        else
                            err = -1;
                    break;
                }
                
                local_spinlock_unlock(&thrd->lock);
                local_spinlock_unlock(&qs[i].queue_lock);
                return err;
            }
            local_spinlock_unlock(&thrd->lock);
            thrd = thrd->next;
        }
        local_spinlock_unlock(&qs[i].queue_lock);
    }
    return -1;
}

//terminals are only deleted by the cpu that owns them
//delete all terminals pending on this cpu
void terminal_delete(void) {
    //lock core queue first, remove the terminals from it and unlock
    
    terminaldef_t *d_term = NULL;
    local_spinlock_lock(&qs[cur_state->coreIdx].queue_lock);
    
    terminaldef_t *thrd = qs[cur_state->coreIdx].queue;
    terminaldef_t *prev = NULL;
        
    while(thrd != NULL) {        
        local_spinlock_lock(&thrd->lock);
        terminaldef_t *n_term = thrd->next;
        if(thrd->state == terminalstate_exiting){
            if(prev != NULL)prev->next = thrd->next; //Remove thrd from the thread list
            
            //Add thrd to the delete list
            thrd->next = d_term;
            d_term = thrd;

            //If thrd is removed from the list, prev remains the same
        }else{
            prev = thrd;
        }
        local_spinlock_unlock(&thrd->lock);
        thrd = n_term;
    }

    local_spinlock_unlock(&qs[cur_state->coreIdx].queue_lock);

    //TODO: Free the term state now that the terminals have been completely removed from the schedule
} 

int module_init(){
    qs = malloc(sizeof(queue_state_t) * mp_corecount());
    memset(qs, 0, sizeof(queue_state_t*) * mp_corecount());

    if(cur_state == NULL)
        cur_state = (TLS terminal_state_t*)mp_tls_get(mp_tls_alloc(sizeof(terminal_state_t)));

    return 0;
}

int terminal_mp_init() {
    cur_state->coreIdx = coreId++; 

    while(1) {
        //attempt to obtain a task
        //If tasks aren't obtained, we should hlt the cpu after setting the apic timer
    }

    return 0;
}