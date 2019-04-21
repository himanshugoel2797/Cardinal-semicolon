// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_IPC_SYSCALL_LIB_H
#define CARDINAL_IPC_SYSCALL_LIB_H

#include <stdint.h>
#include "error.h"
#include "thread.h"

typedef enum {
    cs_pipe_flags_none = 0,
    cs_pipe_flags_read = 1 << 1,
    cs_pipe_flags_write = 1 << 2,
} cs_pipe_flags_t;

typedef uint64_t pipe_t;

cs_error cs_createpipe(const char *name, const char *capability_name, uint32_t sz, cs_pipe_flags_t flags);
cs_error cs_openpipe(const char *name, intptr_t *mem, pipe_t* pipe_id);
cs_error cs_closepipe(pipe_t pipe_id);

cs_error cs_createcapability(const char *capability_name);
cs_error cs_sharecapability(cs_id dst, const char *capability_name);

#endif