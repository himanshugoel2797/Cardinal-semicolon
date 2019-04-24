// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSCALLS_THREAD_LIB_H
#define CARDINAL_SYSCALLS_THREAD_LIB_H

#include <stdint.h>
#include "error.h"

typedef uint64_t cs_id;

cs_error cs_nanosleep(uint64_t ns);

#endif