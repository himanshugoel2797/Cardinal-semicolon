// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSCALLS_THREAD_LIB_H
#define CARDINAL_SYSCALLS_THREAD_LIB_H

#include <stdint.h>
#include "error.h"

#define CS_SIGNAL_COUNT (512)

typedef enum {
    cs_task_type_process,
    cs_task_type_thread,
} cs_task_type;

typedef uint64_t cs_id;
typedef uint32_t cs_signalnum;
typedef void (*cs_signalhandler)(cs_signalnum signal, int err);

cs_error cs_createtask(cs_task_type tasktype, char *name, cs_id *id);
cs_error cs_starttask(cs_id id, void *start_address, void *argument);

cs_error cs_nanosleep(uint64_t ns);

cs_error cs_signal(cs_signalnum signal);
cs_error cs_registersignal(cs_signalnum signal, cs_signalhandler handler);
cs_error cs_signalmask(int mask);

cs_error cs_exit(int ret_code);
cs_error cs_killtask(cs_id id);

#endif