// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TASKMGR_TASK_H
#define CARDINAL_TASKMGR_TASK_H

#include <stdint.h>
#include "cs_syscall.h"

typedef void (*DescriptorResourceFreeAction)(void *);

typedef enum
{
    task_permissions_none = 0,
    task_permissions_kernel = 1,
} task_permissions_t;

typedef enum
{
    task_map_none = 0,
    task_map_oneway = 1 << 2,
    task_map_shared = 1 << 3,
    task_map_oneuse = 1 << 4,
} task_map_flags_t;

typedef enum
{
    task_map_perm_writeonly = 1 << 0,
    task_map_perm_execute = 1 << 1,
    task_map_perm_cachewritethrough = 1 << 2,
    task_map_perm_cachewriteback = 1 << 3,
    task_map_perm_cachewritecomplete = 1 << 4,
    task_map_perm_uncached = 1 << 5,
} task_map_perms_t;

cs_error create_task_kernel(char *name, task_permissions_t perms, cs_id *id);

cs_error start_task_kernel(cs_id id, void (*handler)(void *arg), void *argval);

cs_error end_task_kernel(cs_id id);

cs_error task_map(cs_id id, const char *name, intptr_t vaddr, size_t sz, task_map_flags_t flags, task_map_perms_t owner_perms, task_map_perms_t child_perms, int child_count, cs_id *shmem_id);

cs_error task_virttophys(cs_id id, intptr_t vaddr, intptr_t *phys);

cs_error task_updatemap(cs_id id, cs_id shmem_id, task_map_perms_t perms);

cs_error task_unmap(cs_id id, cs_id shmem_id);

cs_error task_allocdescriptor(cs_id id, DescriptorResourceFreeAction action, void *state, cs_id *descriptor);

cs_error task_freedescriptor(cs_id id, cs_id descriptor);



#endif