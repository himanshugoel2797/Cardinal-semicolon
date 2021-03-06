// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_KVS_LIB_H
#define CARDINAL_KVS_LIB_H

#include <stdbool.h>
#include <stdint.h>
#include <types.h>

typedef enum {
    kvs_val_uninit = -1,
    kvs_val_none = 0,
    kvs_val_sint = 1,
    kvs_val_uint = 2,
    kvs_val_str = 3,
    kvs_val_ptr = 4,
    kvs_val_child = 5,
    kvs_val_bool = 6,
} kvs_val_type;

typedef enum {
    kvs_ok = 0,
    kvs_error_unkwn = 1,
    kvs_error_outofmemory = 2,
    kvs_error_invalidargs = 3,
    kvs_error_notfound = 4,
    kvs_error_exists = 5,
} kvs_error;

#define key_len 228

typedef struct kvs {
    char key[key_len];
    bool owner_locked;
    uint32_t key_hash;
    int val_type;
    union {
        int64_t s_val;
        uint64_t u_val;
        char *str;
        void *ptr;
        bool b_val;
        struct kvs *child;
    };
    struct kvs *next;
} kvs_t;

int kvs_create(kvs_t **r);

int kvs_islocked(kvs_t *r, bool *status);
int kvs_lockentry(kvs_t *r);
int kvs_unlockentry(kvs_t *r);

int kvs_add_sint(kvs_t *r, const char *key, int64_t sval);
int kvs_add_bool(kvs_t *r, const char *key, bool sval);
int kvs_add_uint(kvs_t *r, const char *key, uint64_t uval);
int kvs_add_str(kvs_t *r, const char *key, char *strval);
int kvs_add_ptr(kvs_t *r, const char *key, void *ptrval);
int kvs_add_child(kvs_t *r, const char *key, kvs_t *childval);

int kvs_find(kvs_t *r, const char *key, kvs_t **res);

int kvs_next(kvs_t **r);

int kvs_get_key(kvs_t *r, char *key);
int kvs_get_ptr(kvs_t *r, uintptr_t *key);
int kvs_get_uint(kvs_t *r, uint64_t *key);
int kvs_get_bool(kvs_t *r, bool *key);
int kvs_get_sint(kvs_t *r, int64_t *key);
int kvs_get_str(kvs_t *r, char **key);
int kvs_get_child(kvs_t *r, kvs_t **key);

int kvs_set_key(kvs_t *r, char *key);
int kvs_set_ptr(kvs_t *r, uintptr_t key);
int kvs_set_uint(kvs_t *r, uint64_t key);
int kvs_set_bool(kvs_t *r, bool key);
int kvs_set_sint(kvs_t *r, int64_t key);
int kvs_set_str(kvs_t *r, char *key);

int kvs_get_type(kvs_t *idx, kvs_val_type *val_type);

int kvs_remove(kvs_t *r, kvs_t *idx);
int kvs_delete(kvs_t *r);

#endif