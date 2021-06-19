/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "kvs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#define FNV1A_BASIS 2166136261
#define FNV1A_PRIME 16777619
static uint32_t hash(const char *src, size_t src_len) {
    uint32_t hash = FNV1A_BASIS;
    for (size_t i = 0; i < src_len; i++) {
        hash ^= src[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

int kvs_create(kvs_t **r NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    kvs_t *k = malloc(sizeof(kvs_t));
    if (k == NULL)
        return kvs_error_outofmemory;

    k->owner_locked = false;
    k->val_type = kvs_val_uninit;
    k->next = NULL;
    *r = k;
    return kvs_ok;
}

int kvs_islocked(kvs_t *r NULLABLE, bool *status) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (status == NULL)
        return kvs_error_invalidargs;

    *status = r->owner_locked;
    return kvs_ok;
}

int kvs_lockentry(kvs_t *r NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    r->owner_locked = true;
    return kvs_ok;
}

int kvs_unlockentry(kvs_t *r NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    r->owner_locked = false;
    return kvs_ok;
}

static int kvs_add_internal(kvs_t *r NULLABLE, const char *key, void *val,
                            kvs_val_type val_type) {

    if (r == NULL)
        return kvs_error_invalidargs;

    if (kvs_find(r, key, NULL) == kvs_ok)
        return kvs_error_exists;

    uint32_t key_hash = hash(key, strnlen(key, key_len));

    kvs_t *v = malloc(sizeof(kvs_t));
    if (v == NULL)
        return kvs_error_outofmemory;

    strncpy(v->key, key, key_len);
    v->owner_locked = false;
    v->key_hash = key_hash;
    v->val_type = val_type;
    v->ptr = val;

    v->next = r->next;
    r->next = v;

    return kvs_ok;
}

int kvs_add_sint(kvs_t *r, const char *key, int64_t sval) {
    return kvs_add_internal(r, key, (void *)sval, kvs_val_sint);
}

int kvs_add_bool(kvs_t *r, const char *key, bool sval) {
    return kvs_add_internal(r, key, (void *)(uint64_t)sval, kvs_val_bool);
}

int kvs_add_uint(kvs_t *r NULLABLE, const char *key, uint64_t uval) {
    return kvs_add_internal(r, key, (void *)uval, kvs_val_uint);
}

int kvs_add_str(kvs_t *r NULLABLE, const char *key, char *strval NULLABLE) {
    return kvs_add_internal(r, key, (void *)strval, kvs_val_str);
}

int kvs_add_ptr(kvs_t *r NULLABLE, const char *key, void *ptrval NULLABLE) {
    return kvs_add_internal(r, key, (void *)ptrval, kvs_val_ptr);
}

int kvs_add_child(kvs_t *r NULLABLE, const char *key,
                  kvs_t *childval NULLABLE) {
    return kvs_add_internal(r, key, (void *)childval, kvs_val_child);
}


int kvs_next(kvs_t **r NULLABLE) {
    if(r != NULL && *r != NULL && (*r)->next != NULL) {
        *r = (*r)->next;
        return kvs_ok;
    }
    return kvs_error_notfound;
}

static int kvs_get_internal(kvs_t *r NULLABLE, uint64_t *key, kvs_val_type valType) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (key == NULL)
        return kvs_error_invalidargs;

    if (r->val_type != valType)
        return kvs_error_invalidargs;

    *key = r->u_val;
    return kvs_ok;
}

int kvs_get_key(kvs_t *r NULLABLE, char *key NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (key == NULL)
        return kvs_error_invalidargs;

    strncpy(key, r->key, key_len);
    return kvs_ok;
}

int kvs_get_ptr(kvs_t *r NULLABLE, uintptr_t *key NULLABLE) {
    return kvs_get_internal(r, (uint64_t*)key, kvs_val_ptr);
}

int kvs_get_uint(kvs_t *r NULLABLE, uint64_t *key NULLABLE) {
    return kvs_get_internal(r, (uint64_t*)key, kvs_val_uint);
}

int kvs_get_bool(kvs_t *r NULLABLE, bool *key NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (key == NULL)
        return kvs_error_invalidargs;

    if (r->val_type != kvs_val_bool)
        return kvs_error_invalidargs;

    *key = r->b_val;
    return kvs_ok;
}

int kvs_get_sint(kvs_t *r NULLABLE, int64_t *key NULLABLE) {
    return kvs_get_internal(r, (uint64_t*)key, kvs_val_sint);
}

int kvs_get_str(kvs_t *r NULLABLE, char **key NULLABLE) {
    return kvs_get_internal(r, (uint64_t*)key, kvs_val_str);
}

int kvs_get_child(kvs_t *r NULLABLE, kvs_t **key NULLABLE) {
    return kvs_get_internal(r, (uint64_t*)key, kvs_val_child);
}


static int kvs_set_internal(kvs_t *r NULLABLE, uint64_t key, kvs_val_type valType) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (r->val_type != valType)
        return kvs_error_invalidargs;

    r->u_val = key;
    return kvs_ok;
}

int kvs_set_key(kvs_t *r NULLABLE, char *key NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (key == NULL)
        return kvs_error_invalidargs;

    strncpy(r->key, key, key_len);
    return kvs_ok;
}

int kvs_set_ptr(kvs_t *r NULLABLE, uintptr_t key NULLABLE) {
    return kvs_set_internal(r, (uint64_t)key, kvs_val_ptr);
}

int kvs_set_uint(kvs_t *r NULLABLE, uint64_t key NULLABLE) {
    return kvs_set_internal(r, key, kvs_val_uint);
}

int kvs_set_bool(kvs_t *r NULLABLE, bool key NULLABLE) {
    return kvs_set_internal(r, (uint64_t)key, kvs_val_bool);
}

int kvs_set_sint(kvs_t *r NULLABLE, int64_t key NULLABLE) {
    return kvs_set_internal(r, (uint64_t)key, kvs_val_sint);
}

int kvs_set_str(kvs_t *r NULLABLE, char *key NULLABLE) {
    return kvs_set_internal(r, (uint64_t)key, kvs_val_str);
}

int kvs_find(kvs_t *r NULLABLE, const char *key, kvs_t **res) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (key == NULL)
        return kvs_error_invalidargs;

    uint32_t key_hash = hash(key, strnlen(key, key_len));

    kvs_t *iter = r;
    do {
        if (iter->key_hash == key_hash && iter->val_type != kvs_val_uninit) {
            if (strncmp(key, iter->key, MIN(strnlen(iter->key, key_len),
                                            strnlen(key, key_len))) == 0) {
                if (res != NULL)
                    *res = iter;
                return kvs_ok;
            }
        }
        iter = iter->next;
    } while (iter != NULL);

    return kvs_error_notfound;
}

int kvs_get_type(kvs_t *idx NULLABLE,
                 kvs_val_type *val_type NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (val_type == NULL)
        return kvs_error_invalidargs;

    *val_type = idx->val_type;
    return kvs_ok;
}

int kvs_remove(kvs_t *r NULLABLE, kvs_t *idx NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    if (idx == NULL)
        return kvs_error_invalidargs;

    kvs_t *iter = r;
    do {
        iter = iter->next;
        if (iter == NULL)
            return kvs_error_notfound;
    } while (iter->next != idx);

    iter->next = idx->next;
    if(idx->val_type == kvs_val_child)
        kvs_delete(idx->child);
    free(idx);

    return kvs_ok;
}

int kvs_delete(kvs_t *r NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    kvs_t *iter = r;
    do {
        kvs_t *n = iter->next;
        if(iter->val_type == kvs_val_child)
            kvs_delete(iter->child);
        free(iter);
        iter = n;
    } while (iter != NULL);

    return kvs_ok;
}