// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSCALLS_CS_LIB_H
#define CARDINAL_SYSCALLS_CS_LIB_H

#include <stdint.h>
#include "error.h"

static __inline uint64_t cs_syscall0(uint64_t s, uint64_t n)
{
	uint64_t ret;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
    __asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13) : "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall1(uint64_t s, uint64_t n, uint64_t a1)
{
	uint64_t ret;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1) : "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall2(uint64_t s, uint64_t n, uint64_t a1, uint64_t a2)
{
	uint64_t ret;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1), "S"(a2)
						  : "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall3(uint64_t s, uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint64_t ret;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1), "S"(a2),
						  "d"(a3) : "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall4(uint64_t s, uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	uint64_t ret;
	register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10): "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall5(uint64_t s, uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	uint64_t ret;
	register uint64_t r10 __asm__("r10") = a4;
	register uint64_t r8 __asm__("r8") = a5;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
	return ret;
}

static __inline uint64_t cs_syscall6(uint64_t s, uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
	uint64_t ret;
	register uint64_t r10 __asm__("r10") = a4;
	register uint64_t r8 __asm__("r8") = a5;
	register uint64_t r9 __asm__("r9") = a6;
    register uint64_t r12 __asm__("r12") = n;
    register uint64_t r13 __asm__("r13") = s;
	__asm__ __volatile__ ("syscallq" : "=a"(ret) : "r"(r12), "r"(r13), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return ret;
}

typedef uint64_t cs_id;

#define CS_SYSCALLSET_DEFAULT 0

#define CS_SYSCALL_NANOSLEEP 0
#define CS_SYSCALL_ENDTASK 1
#define CS_SYSCALL_OPENSPECIALSET 2

static __inline cs_error cs_nanosleep(uint64_t ns) {
    return cs_syscall1(CS_SYSCALLSET_DEFAULT, CS_SYSCALL_NANOSLEEP, ns);
}

static __inline cs_error cs_endtask() {
    return cs_syscall0(CS_SYSCALLSET_DEFAULT, CS_SYSCALL_ENDTASK);
}

static __inline cs_error cs_openspecialset(uint32_t set_id, uint32_t *call_idx) {
    return cs_syscall2(CS_SYSCALLSET_DEFAULT, CS_SYSCALL_OPENSPECIALSET, set_id, (uint64_t)call_idx);
}

#endif