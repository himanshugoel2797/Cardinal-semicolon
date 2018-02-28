// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSUSER_SYSCALL_H
#define CARDINAL_SYSUSER_SYSCALL_H

#include <types.h>

int syscall_sethandler(int idx, void* func);
PURE int syscall_parameterreg_count(void);
void syscall_setstate(uint64_t rsp, uint64_t rip, uint64_t rflags);
void syscall_touser(uint64_t *regs);

void syscall_getfullstate(void* dst);
void syscall_setfullstate(void* state);
PURE int syscall_getfullstate_size(void);

#endif