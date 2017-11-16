/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdqueue.h>
#include <stdlib.h>
#include <stdbool.h>
#include <cardinal/local_spinlock.h>

int queue_init(queue_t *q, int32_t sz) {
    if(q == NULL)
        return -1;

    q->queue = malloc(sz * sizeof(uint64_t));

    if(q->queue == NULL)
        return -1;

    q->size = sz;
    q->ent_cnt = 0;
    q->head = 0;
    q->tail = 0;

    return 0;
}

void queue_fini(queue_t *q) {
    if(q == NULL)
        return;

    free(q->queue);
    q->size = 0;
}

int32_t queue_size(queue_t *q) {
    if(q == NULL)
        return 0;

    return q->size;
}

int32_t queue_entcnt(queue_t *q) {
    if(q == NULL)
        return 0;

    return q->ent_cnt;
}

bool queue_tryenqueue(queue_t *q, uint64_t val) {
    if(q == NULL)
        return false;

    int32_t curTail = q->tail;
    if (q->head == (curTail + 1) % q->size)
        return false;

    q->queue[curTail] = val;
    q->tail = (curTail + 1) % q->size;
    q->ent_cnt++;

    return true;
}

bool queue_trydequeue(queue_t *q, uint64_t *val) {
    if(q == NULL)
        return false;

    int32_t curHead = q->head;
    if (curHead == q->tail)
        return false;

    *val = q->queue[curHead];
    q->queue[curHead] = 0;

    q->head = (curHead + 1) % q->size;
    q->ent_cnt--;

    return true;
}