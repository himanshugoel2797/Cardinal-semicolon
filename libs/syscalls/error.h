// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_ERROR_SYSCALL_LIB_H
#define CARDINAL_ERROR_SYSCALL_LIB_H

// cs_error and the CS_* codes now live in the globally-available common header
// so every module can use them without a libs/syscalls include. Kept here for
// existing includers of <error.h> / cs_syscall.h.
#include <cardinal/cs_error.h>

#endif