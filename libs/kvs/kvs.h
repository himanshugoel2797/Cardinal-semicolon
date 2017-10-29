// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_KVS_LIB_H
#define CARDINAL_KVS_LIB_H

#include <stdint.h>

typedef enum {
  kvs_val_uninit = -1,
  kvs_val_none = 0,
  kvs_val_sint = 1,
  kvs_val_uint = 2,
  kvs_val_str = 3,
  kvs_val_ptr = 4,
  kvs_val_child = 5,
} kvs_val_type;

typedef enum {
  kvs_ok = 0,
  kvs_error_unkwn = 1,
  kvs_error_outofmemory = 2,
  kvs_error_invalidargs = 3,
  kvs_error_notfound = 4,
  kvs_error_exists = 5,
} kvs_error;

#define key_len 232

typedef struct kvs {
  char key[key_len];
  uint32_t key_hash;
  int val_type;
  union {
    int64_t s_val;
    uint64_t u_val;
    char *str;
    void *ptr;
    struct kvs *child;
  };
  struct kvs *next;
} kvs_t;

int kvs_create(kvs_t **r);

int kvs_add_sint(kvs_t *r, const char *key, int64_t sval);
int kvs_add_uint(kvs_t *r, const char *key, uint64_t uval);
int kvs_add_str(kvs_t *r, const char *key, char *strval);
int kvs_add_ptr(kvs_t *r, const char *key, void *ptrval);
int kvs_add_child(kvs_t *r, const char *key, kvs_t *childval);

int kvs_find(kvs_t *r, const char *key, kvs_t **res);

int kvs_get_type(kvs_t *r, kvs_t *idx, kvs_val_type *val_type);

int kvs_get_sint(kvs_t *r, kvs_t *idx, int64_t *sval);
int kvs_get_uint(kvs_t *r, kvs_t *idx, uint64_t *sval);
int kvs_get_str(kvs_t *r, kvs_t *idx, char **sval);
int kvs_get_ptr(kvs_t *r, kvs_t *idx, void **sval);
int kvs_get_child(kvs_t *r, kvs_t *idx, kvs_t **sval);

int kvs_remove(kvs_t *r, kvs_t *idx);
int kvs_delete(kvs_t *r);

#endif