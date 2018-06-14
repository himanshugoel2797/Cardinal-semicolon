// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TASKMGR_TASK_H
#define CARDINAL_TASKMGR_TASK_H

#include <stdint.h>
#include "thread.h"

cs_error create_task_kernel(cs_task_type tasktype, char *name, task_permissions_t perms, cs_id *id);

cs_error start_task_kernel(cs_id id, void(*handler)(void *arg));

#endif