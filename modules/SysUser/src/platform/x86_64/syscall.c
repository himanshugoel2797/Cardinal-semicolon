/**
 * Copyright (c) 2018 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <types.h>

#include "SysMP/mp.h"

typedef struct {
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rip;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t rflags;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} register_state_t;

typedef struct {
    void *kernel_stack;
    void *user_stack;
    register_state_t registers;
} syscall_state_t;
TLS syscall_state_t *syscall_state = NULL;

PRIVATE NAKED NORETURN void syscall_handler(void) {
    //Save the register state to tls space
    __asm__ volatile(
        "movq %rsp, %rax\r\n"
        "movq (syscall_state), %rsp\r\n"
        "movq %rax, %gs:+0x10(%rsp)\r\n"  //RSP
        "movq %rbp, %gs:+0x18(%rsp)\r\n"  //RBP
        "movq %rbx, %gs:+0x20(%rsp)\r\n"  //RBX
        "movq %rcx, %gs:+0x28(%rsp)\r\n"  //RIP
        "movq %rdx, %gs:+0x30(%rsp)\r\n"  //RDX
        "movq %rsi, %gs:+0x38(%rsp)\r\n"  //RSI
        "movq %rdi, %gs:+0x40(%rsp)\r\n"  //RDI
        "movq %r8, %gs:+0x48(%rsp)\r\n"  //R8
        "movq %r9, %gs:+0x50(%rsp)\r\n"  //R9
        "movq %r10, %gs:+0x58(%rsp)\r\n"  //R10
        "movq %r11, %gs:+0x60(%rsp)\r\n"  //RFLAGS
        "movq %r12, %gs:+0x68(%rsp)\r\n"  //R12
        "movq %r13, %gs:+0x70(%rsp)\r\n"  //R13
        "movq %r14, %gs:+0x78(%rsp)\r\n"  //R14
        "movq %r15, %gs:+0x80(%rsp)\r\n"  //R15
        "movq %gs:(%rsp), %rsp\r\n" //Load the kernel stack pointer

        //Call the syscall function


        //Restore the stored state
        "movq (syscall_state), %rsp\r\n"
        "movq %gs:+0x80(%rsp), %r15\r\n"    //R15
        "movq %gs:+0x78(%rsp), %r14\r\n"    //R14
        "movq %gs:+0x70(%rsp), %r13\r\n"    //R13
        "movq %gs:+0x68(%rsp), %r12\r\n"    //R12
        "movq %gs:+0x60(%rsp), %r11\r\n"    //RFLAGS
        "movq %gs:+0x58(%rsp), %r10\r\n"    //R10
        "movq %gs:+0x50(%rsp), %r9\r\n"    //R9
        "movq %gs:+0x48(%rsp), %r8\r\n"    //R8
        "movq %gs:+0x40(%rsp), %rdi\r\n"    //RDI
        "movq %gs:+0x38(%rsp), %rsi\r\n"    //RSI
        "movq %gs:+0x30(%rsp), %rdx\r\n"    //RDX
        "movq %gs:+0x28(%rsp), %rcx\r\n"    //RIP
        "movq %gs:+0x20(%rsp), %rbx\r\n"    //RDX
        "movq %gs:+0x18(%rsp), %rbp\r\n"    //RBP
        "movq %gs:+0x10(%rsp), %rsp\r\n"    //RSP
        "sysretq\r\n"
    );
}

//Processes are started and functions called via syscall exit

PRIVATE int syscall_plat_init(){

    if(syscall_state == NULL)
        syscall_state = (TLS syscall_state_t*)mp_tls_get(mp_tls_alloc(sizeof(syscall_state_t)));

    uint64_t star_val = (0x08ull << 32) | (0x18ull << 48);
    uint64_t lstar = (uint64_t)syscall_handler;
    uint64_t sfmask = (uint64_t)-1;

    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);  // Enable the syscall instruction
    wrmsr(0xC0000081, star_val);
    wrmsr(0xC0000082, lstar);
    wrmsr(0xC0000084, sfmask);

    return 0;
}