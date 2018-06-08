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
    cs_ipc_type_none = 0,
    cs_ipc_type_short,
    cs_ipc_type_string,
    cs_ipc_type_grant,
    cs_ipc_type_map,
} cs_ipc_type;

cs_error cs_sendipc(cs_ipc_type type, void *data, cs_id dst_id);
cs_error cs_receiveipc();

#endif