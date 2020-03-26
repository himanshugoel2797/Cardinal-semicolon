/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "SysMP/mp.h"

typedef struct
{
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} register_state_t;

typedef struct
{
    void *kernel_stack;
    register_state_t registers;
} syscall_state_t;
TLS syscall_state_t *syscall_state = NULL;

#define SYSCALL_COUNT 256
static void *syscall_funcs[SYSCALL_COUNT];

PRIVATE NAKED NORETURN void syscall_handler(void)
{
    //Save the register state to tls space
    __asm__ volatile(

        //"swapgs\r\n"
        "cmp $" S_(SYSCALL_COUNT) ", %r12\r\n"
                                  "jge exit_syscall_handler\r\n"

                                  "movq %rsp, %rax\r\n"
                                  "movq (syscall_state), %rsp\r\n"
                                  "movq %rcx, %gs:0x8(%rsp)\r\n"  //RIP
                                  "movq %r11, %gs:0x10(%rsp)\r\n" //RFLAGS
                                  "movq %rax, %gs:0x18(%rsp)\r\n" //RSP
                                  "movq %rbp, %gs:0x20(%rsp)\r\n" //RBP
                                  "movq %rbx, %gs:0x28(%rsp)\r\n" //RBX
                                  "movq %r12, %gs:0x30(%rsp)\r\n" //R12
                                  "movq %r13, %gs:0x38(%rsp)\r\n" //R13
                                  "movq %r14, %gs:0x40(%rsp)\r\n" //R14
                                  "movq %r15, %gs:0x48(%rsp)\r\n" //R15
                                  "movq %gs:(%rsp), %rsp\r\n"     //Load the kernel stack pointer

                                  //Call the syscall function
                                  "movq $syscall_funcs, %rax\r\n"
                                  "movq (%rax, %r12, 8), %rax\r\n" //Call Syscalls[%r12]
                                  "callq *%rax\r\n"

                                  //Restore the stored state
                                  "movq (syscall_state), %rsp\r\n"
                                  "movq %gs:0x48(%rsp), %r15\r\n" //R15
                                  "movq %gs:0x40(%rsp), %r14\r\n" //R14
                                  "movq %gs:0x38(%rsp), %r13\r\n" //R13
                                  "movq %gs:0x30(%rsp), %r12\r\n" //R12
                                  "movq %gs:0x28(%rsp), %rbx\r\n" //RBX
                                  "movq %gs:0x20(%rsp), %rbp\r\n" //RBP
                                  "movq %gs:0x10(%rsp), %r11\r\n" //RFLAGS
                                  "movq %gs:0x8(%rsp), %rcx\r\n"  //RIP
                                  "movq %gs:0x18(%rsp), %rsp\r\n" //RSP

                                  "exit_syscall_handler:\r\n"
                                  //"swapgs\r\n"
                                  "sysretq\r\n");
}

//Processes are started and functions called via syscall exit
PRIVATE NAKED NORETURN void user_transition(void UNUSED(*stack_base))
{
    __asm__ volatile(
        //Restore the stored state
        "cli\r\n"
        "movq (syscall_state), %rsp\r\n"
        "movq %gs:0x48(%rsp), %r15\r\n" //R15
        "movq %gs:0x40(%rsp), %r14\r\n" //R14
        "movq %gs:0x38(%rsp), %r13\r\n" //R13
        "movq %gs:0x30(%rsp), %r12\r\n" //R12
        "movq %gs:0x28(%rsp), %rbx\r\n" //RBX
        "movq %gs:0x20(%rsp), %rbp\r\n" //RBP
        "movq %gs:0x10(%rsp), %r11\r\n" //RFLAGS
        "movq %gs:0x8(%rsp), %rcx\r\n"  //RIP
        "movq %gs:0x18(%rsp), %rsp\r\n" //RSP
        //"hlt\r\n"
        "swapgs\r\n"
        "sysretq\r\n");
}

int syscall_sethandler(int idx, void *func)
{
    if (idx < SYSCALL_COUNT)
    {
        syscall_funcs[idx] = func;
        return 0;
    }

    return -1;
}

void syscall_getfullstate(void *dst)
{
    syscall_state_t *st = (syscall_state_t *)dst;
    st->kernel_stack = syscall_state->kernel_stack;
    st->registers = syscall_state->registers;
}

void syscall_setfullstate(void *state)
{
    syscall_state_t *st = (syscall_state_t *)state;
    syscall_state->kernel_stack = st->kernel_stack;
    syscall_state->registers = st->registers;
}

void syscall_getdefaultstate(void *state, void *kernel_stack, void *user_stack, void *rip)
{
    syscall_state_t *st = (syscall_state_t *)state;
    st->kernel_stack = kernel_stack;
    st->registers.rsp = (uint64_t)user_stack;
    st->registers.rflags = 0x3200;
    st->registers.rip = (uint64_t)rip;
    st->registers.rbp = (uint64_t)user_stack;
    st->registers.rbx = 0;
    st->registers.r12 = 0;
    st->registers.r13 = 0;
    st->registers.r14 = 0;
    st->registers.r15 = 0;
}

PURE int syscall_getfullstate_size(void)
{
    return sizeof(syscall_state_t);
}

void syscall_touser(void *arg)
{
    user_transition(arg);
}

PRIVATE int syscall_plat_init()
{
    if (syscall_state == NULL)
        syscall_state = (TLS syscall_state_t *)mp_tls_get(mp_tls_alloc(sizeof(syscall_state_t)));

    uint64_t star_val = (0x08ull << 32) | (0x18ull << 48);
    uint64_t lstar = (uint64_t)syscall_handler;
    uint64_t sfmask = (uint64_t)-1;

    wrmsr(EFER_MSR, rdmsr(EFER_MSR) | 1); // Enable the syscall instruction
    wrmsr(0xC0000081, star_val);
    wrmsr(0xC0000082, lstar);
    wrmsr(0xC0000084, sfmask);

    return 0;
}