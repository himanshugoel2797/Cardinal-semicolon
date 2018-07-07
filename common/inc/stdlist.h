// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _CARDINAL_LIST_H_
#define _CARDINAL_LIST_H_

#include <stdint.h>

typedef struct list_node list_node_t;
typedef struct list list_t;

typedef struct list_node {
    void *value;
    list_node_t *next;
    list_node_t *prev;
} list_node_t;

typedef struct list {
    list_node_t *nodes;
    list_node_t *last;
    list_node_t *last_accessed_node;
    uint64_t last_accessed_index;
    uint64_t entry_count;
} list_t;


typedef enum list_error {
    list_error_none = 0,
    list_error_allocation_failed = (1 << 0),
    list_error_eol = (1 << 1),
    list_error_deleting = (1 << 2),
} list_error_t;

int
list_init(list_t *list);

list_error_t
list_append(list_t *a, void *value);

uint64_t
list_len(list_t *a);

void
list_remove(list_t *a,
            uint64_t index);

void
list_fini(list_t *a);

void*
list_at(list_t *a,
        uint64_t index);

void*
list_rot_next(list_t *a);

void*
list_rot_prev(list_t *a);

uint64_t
list_history(list_t *a);

#endif