// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSOBJ_H
#define CARDINAL_SYSOBJ_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <cardinal/cs_error.h>
#include <dir_t.h>

#define MAX_OBJ_DEPTH (20)
#define MAX_OBJ_KEYLEN (200)
#define MAX_OBJ_STRLEN (4096)

// Error codes are the unified cs_error set (common/inc/cardinal/cs_error.h).
//
// Naming convention: this is a key/value store, so accessors use read/write
// (addkey/readkey/writekey), distinct from the get/set verbs used for scalar
// hardware/CPU state. SysReg mirrors this read/write convention.

cs_error obj_createdirectory(const char *path, const char *dirname);

cs_error obj_addkey_uint(const char *path, const char *keyname, uint64_t val);

cs_error obj_addkey_ptr(const char *path, const char *keyname, uintptr_t val);

cs_error obj_addkey_int(const char *path, const char *keyname, int64_t val);

cs_error obj_addkey_str(const char *path, const char *keyname, const char *val);

cs_error obj_addkey_bool(const char *path, const char *keyname, bool val);

cs_error obj_readkey_uint(const char *path, const char *keyname, uint64_t *val);

cs_error obj_readkey_ptr(const char *path, const char *keyname, uintptr_t *val);

cs_error obj_readkey_int(const char *path, const char *keyname, int64_t *val);

cs_error obj_readkey_str(const char *path, const char *keyname, char *val,
                         size_t *val_len);

cs_error obj_readkey_bool(const char *path, const char *keyname, bool *val);

cs_error obj_writekey_uint(const char *path, const char *keyname, uint64_t val);

cs_error obj_writekey_ptr(const char *path, const char *keyname, uintptr_t val);

cs_error obj_writekey_int(const char *path, const char *keyname, int64_t val);

cs_error obj_writekey_str(const char *path, const char *keyname, const char *val);

cs_error obj_writekey_bool(const char *path, const char *keyname, bool val);

cs_error obj_removekey(const char *path, const char *keyname);

cs_error obj_removedirectory(const char *path, const char *dirname);

cs_error obj_getdirectory(const char *path, dir_t *dir);

cs_error obj_next(dir_t *dir);

cs_error obj_readlocal_key(dir_t dir, char *keyname);

cs_error obj_readlocal_uint(dir_t dir, uint64_t *val);

cs_error obj_readlocal_ptr(dir_t dir, void **val);

cs_error obj_readlocal_int(dir_t dir, int64_t *val);

cs_error obj_readlocal_str(dir_t dir, char **val);

cs_error obj_readlocal_bool(dir_t dir, bool *val);

cs_error obj_readlocal_dir(dir_t dir, dir_t *val);

cs_error obj_lock(dir_t dir);

cs_error obj_unlock(dir_t dir);

cs_error obj_islocked(dir_t dir, bool *status);

#endif