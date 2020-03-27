// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef STD_QUEUE_H
#define STD_QUEUE_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint64_t *queue;
    int32_t size;
    _Atomic int32_t ent_cnt;
    _Atomic volatile int32_t head;
    _Atomic volatile int32_t tail;
} queue_t;

int queue_init(queue_t *q, int32_t sz);

void queue_fini(queue_t *q);

int32_t queue_size(queue_t *q);

int32_t queue_entcnt(queue_t *q);

bool queue_tryenqueue(queue_t *q, uint64_t val);

bool queue_tryenqueue_front(queue_t *q, uint64_t val);

bool queue_trydequeue(queue_t *q, uint64_t *val);

bool queue_peek(queue_t *q, uint64_t *val);

bool queue_full(queue_t *q);

#endif