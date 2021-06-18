// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSOBJ_H
#define CARDINAL_SYSOBJ_H

#include <types.h>

#define MAX_OBJ_DEPTH (20)
#define MAX_OBJ_KEYLEN (200)
#define MAX_OBJ_STRLEN (4096)

typedef void* dir_t;

typedef enum {
    obj_err_ok = 0,
    obj_err_invalidargs = 1,
    obj_err_dne = 2,
    obj_err_exists = 3,
    obj_err_failure = 4,
    obj_err_typematchfailure = 5,
} obj_error;

int obj_createdirectory(const char *path, const char *dirname);

int obj_addkey_uint(const char *path, const char *keyname, uint64_t val);

int obj_addkey_ptr(const char *path, const char *keyname, uintptr_t val);

int obj_addkey_int(const char *path, const char *keyname, int64_t val);

int obj_addkey_str(const char *path, const char *keyname, const char *val);

int obj_addkey_bool(const char *path, const char *keyname, bool val);

int obj_readkey_uint(const char *path, const char *keyname, uint64_t *val);

int obj_readkey_ptr(const char *path, const char *keyname, uintptr_t *val);

int obj_readkey_int(const char *path, const char *keyname, int64_t *val);

int obj_readkey_str(const char *path, const char *keyname, char *val,
                         size_t *val_len);

int obj_readkey_bool(const char *path, const char *keyname, bool *val);

int obj_removekey(const char *path, const char *keyname);

int obj_removedirectory(const char *path, const char *dirname);

int obj_getdirectory(const char *path, dir_t *dir);

int obj_next(dir_t *dir);

int obj_readlocal_key(dir_t dir, char *keyname);

int obj_readlocal_uint(dir_t dir, uint64_t *val);

int obj_readlocal_ptr(dir_t dir, void **val);

int obj_readlocal_int(dir_t dir, int64_t *val);

int obj_readlocal_str(dir_t dir, char **val);

int obj_readlocal_bool(dir_t dir, bool *val);

int obj_readlocal_dir(dir_t dir, dir_t *val);

#endif