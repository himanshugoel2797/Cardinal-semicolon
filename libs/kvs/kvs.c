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

    k->val_type = kvs_val_uninit;
    k->next = NULL;
    *r = k;
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

int kvs_get_type(kvs_t *r UNUSED, kvs_t *idx NULLABLE,
                 kvs_val_type *val_type NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (val_type == NULL)
        return kvs_error_invalidargs;

    *val_type = idx->val_type;
    return kvs_ok;
}

int kvs_get_sint(kvs_t *r UNUSED, kvs_t *idx NULLABLE, int64_t *sval NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (sval == NULL)
        return kvs_error_invalidargs;

    *sval = idx->s_val;
    return kvs_ok;
}

int kvs_get_uint(kvs_t *r UNUSED, kvs_t *idx NULLABLE,
                 uint64_t *sval NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (sval == NULL)
        return kvs_error_invalidargs;

    *sval = idx->u_val;
    return kvs_ok;
}

int kvs_get_str(kvs_t *r UNUSED, kvs_t *idx NULLABLE, char **sval NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (sval == NULL)
        return kvs_error_invalidargs;

    *sval = idx->str;
    return kvs_ok;
}

int kvs_get_ptr(kvs_t *r UNUSED, kvs_t *idx NULLABLE, void **sval NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (sval == NULL)
        return kvs_error_invalidargs;

    *sval = idx->ptr;
    return kvs_ok;
}

int kvs_get_child(kvs_t *r UNUSED, kvs_t *idx NULLABLE, kvs_t **sval NULLABLE) {
    if (idx == NULL)
        return kvs_error_invalidargs;

    if (sval == NULL)
        return kvs_error_invalidargs;

    *sval = idx->child;
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
    free(idx);

    return kvs_ok;
}

int kvs_delete(kvs_t *r NULLABLE) {
    if (r == NULL)
        return kvs_error_invalidargs;

    kvs_t *iter = r;
    do {
        kvs_t *n = iter->next;
        free(iter);
        iter = n;
    } while (iter != NULL);

    return kvs_ok;
}