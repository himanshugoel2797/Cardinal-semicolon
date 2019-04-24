// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_SYSREG_H
#define CARDINAL_SYSREG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_REGISTRY_DEPTH (20)
#define MAX_REGISTRY_KEYLEN (200)
#define MAX_REGISTRY_STRLEN (4096)

typedef void* dir_t;

typedef enum {
    registry_err_ok = 0,
    registry_err_invalidargs = 1,
    registry_err_dne = 2,
    registry_err_exists = 3,
    registry_err_failure = 4,
    registry_err_typematchfailure = 5,
} registry_error;

int registry_createdirectory(const char *path, const char *dirname);

int registry_addkey_uint(const char *path, const char *keyname, uint64_t val);

int registry_addkey_ptr(const char *path, const char *keyname, uintptr_t val);

int registry_addkey_int(const char *path, const char *keyname, int64_t val);

int registry_addkey_str(const char *path, const char *keyname, const char *val);

int registry_addkey_bool(const char *path, const char *keyname, bool val);

int registry_readkey_uint(const char *path, const char *keyname, uint64_t *val);

int registry_readkey_ptr(const char *path, const char *keyname, uintptr_t *val);

int registry_readkey_int(const char *path, const char *keyname, int64_t *val);

int registry_readkey_str(const char *path, const char *keyname, char *val,
                         size_t *val_len);

int registry_readkey_bool(const char *path, const char *keyname, bool *val);

int registry_removekey(const char *path, const char *keyname);

int registry_removedirectory(const char *path, const char *dirname);

int registry_getdirectory(const char *path, dir_t *dir);

int registry_next(dir_t *dir);

int registry_readlocal_key(dir_t dir, const char *keyname);

int registry_readlocal_uint(dir_t dir, uint64_t *val);

int registry_readlocal_ptr(dir_t dir, void **val);

int registry_readlocal_int(dir_t dir, int64_t *val);

int registry_readlocal_str(dir_t dir, const char *val);

int registry_readlocal_bool(dir_t dir, bool *val);

int registry_readlocal_dir(dir_t dir, dir_t *val);

#endif