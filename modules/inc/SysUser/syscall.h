// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSUSER_SYSCALL_H
#define CARDINAL_SYSUSER_SYSCALL_H

#include <types.h>

#define SYSCALL_COUNT 256
#define SYSCALL_SET_COUNT 128

int syscall_sethandler(int idx, void *func);
int syscall_set_syscallset(int idx, void **set);
void** syscall_get_syscallset(int idx);
void syscall_touser(void *arg);

void syscall_getfullstate(void *dst);
void syscall_setfullstate(void *state);
void syscall_getdefaultstate(void *state, void *kernel_stack, void *user_stack, void *rip);
PURE int syscall_getfullstate_size(void);

#endif