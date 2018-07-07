/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <stdlist.h>


int
list_init(list_t *t) {

    t->nodes = NULL;
    t->last = NULL;
    t->last_accessed_node = NULL;
    t->last_accessed_index = 0;
    t->entry_count = 0;

    return 0;
}

list_error_t
list_append(list_t *a,
            void *value) {
    list_node_t *l = malloc(sizeof(list_node_t));
    if(l == NULL)return list_error_allocation_failed;

    l->prev = a->last;
    l->value = value;
    l->next = NULL;
    if(a->last != NULL)a->last->next = l;
    a->last = l;
    if(a->nodes == NULL)a->nodes = a->last;
    if(a->last_accessed_node == NULL) {
        a->last_accessed_node = a->nodes;
        a->last_accessed_index = 0;
    }
    a->entry_count++;

    return list_error_none;
}

uint64_t
list_len(list_t *a) {
    uint64_t val = a->entry_count;
    return val;
}

void
list_remove(list_t *a,
            uint64_t index) {

    if(a->entry_count == 0 | index >= a->entry_count) {
        return;
    }

    if(a->last_accessed_index >= a->entry_count) {
        a->last_accessed_index = 0;
        a->last_accessed_node = a->nodes;
    }

    while(a->last_accessed_index > index) {
        a->last_accessed_node = a->last_accessed_node->prev;
        a->last_accessed_index--;
    }

    while(a->last_accessed_index < index) {
        a->last_accessed_node = a->last_accessed_node->next;
        a->last_accessed_index++;
    }

    void *tmp = a->last_accessed_node;

    if(a->last_accessed_node->prev != NULL)
        a->last_accessed_node->prev->next = a->last_accessed_node->next;

    if(a->last_accessed_node->next != NULL)
        a->last_accessed_node->next->prev = a->last_accessed_node->prev;

    if(a->nodes == a->last_accessed_node)
        a->nodes = a->last_accessed_node->next;

    if(a->last == a->last_accessed_node)
        a->last = a->last_accessed_node->prev;

    a->last_accessed_node = a->nodes;
    a->last_accessed_index = 0;
    free(tmp);

    a->entry_count--;
}

void
list_fini(list_t *a) {
    while(list_len(a) > 0) {
        list_remove(a, 0);
    }
}

void*
list_at(list_t *a,
        uint64_t index) {

    if(a->last_accessed_index >= a->entry_count) {
        a->last_accessed_index = 0;
        a->last_accessed_node = a->nodes;
    }

    if(index >= a->entry_count) {
        return NULL;
    }

    //Accelarate first and last element accesses
    if(index == 0) {
        a->last_accessed_index = 0;
        a->last_accessed_node = a->nodes;
    }

    //Accelerate first and last element accesses
    if(index == a->entry_count - 1) {
        a->last_accessed_index = a->entry_count - 1;
        a->last_accessed_node = a->last;
    }

    while(a->last_accessed_index > index) {
        a->last_accessed_node = a->last_accessed_node->prev;
        a->last_accessed_index--;
    }

    while(a->last_accessed_index < index) {
        a->last_accessed_node = a->last_accessed_node->next;
        a->last_accessed_index++;
    }

    void *val = a->last_accessed_node->value;
    return val;
}

void*
list_rot_next(list_t *a) {

    void *val = NULL;
    if(a->entry_count != 0)
        val = list_at(a, (a->last_accessed_index + 1) % a->entry_count);
    return val;
}

void*
list_rot_prev(list_t *a) {

    void *val = NULL;
    if(a->entry_count != 0)
        val = list_at(a, (a->last_accessed_index - 1) % a->entry_count);
    return val;
}

uint64_t
list_history(list_t *a) {

    uint64_t i = a->last_accessed_index;
    return i;
}