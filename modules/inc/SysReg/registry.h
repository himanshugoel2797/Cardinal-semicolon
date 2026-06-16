// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_SYSREG_H
#define CARDINAL_SYSREG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <cardinal/cs_error.h>
#include <dir_t.h>

#define MAX_REGISTRY_DEPTH (20)
#define MAX_REGISTRY_KEYLEN (200)
#define MAX_REGISTRY_STRLEN (4096)

// Error codes are the unified cs_error set (common/inc/cardinal/cs_error.h).
//
// Naming convention: this is a key/value store, so accessors use read/write
// (addkey/readkey/writekey), distinct from the get/set verbs used for scalar
// hardware/CPU state (SysFP/SysMP/SysInterrupts/SysVirtualMemory). SysObj
// mirrors this read/write convention.

cs_error registry_createdirectory(const char *path, const char *dirname);

cs_error registry_addkey_uint(const char *path, const char *keyname, uint64_t val);

cs_error registry_addkey_ptr(const char *path, const char *keyname, uintptr_t val);

cs_error registry_addkey_int(const char *path, const char *keyname, int64_t val);

cs_error registry_addkey_str(const char *path, const char *keyname, const char *val);

cs_error registry_addkey_bool(const char *path, const char *keyname, bool val);

cs_error registry_readkey_uint(const char *path, const char *keyname, uint64_t *val);

cs_error registry_readkey_ptr(const char *path, const char *keyname, uintptr_t *val);

cs_error registry_readkey_int(const char *path, const char *keyname, int64_t *val);

cs_error registry_readkey_str(const char *path, const char *keyname, char *val,
                              size_t *val_len);

cs_error registry_readkey_bool(const char *path, const char *keyname, bool *val);

cs_error registry_removekey(const char *path, const char *keyname);

cs_error registry_removedirectory(const char *path, const char *dirname);

cs_error registry_getdirectory(const char *path, dir_t *dir);

cs_error registry_next(dir_t *dir);

cs_error registry_readlocal_key(dir_t dir, char *keyname);

cs_error registry_readlocal_uint(dir_t dir, uint64_t *val);

cs_error registry_readlocal_ptr(dir_t dir, void **val);

cs_error registry_readlocal_int(dir_t dir, int64_t *val);

cs_error registry_readlocal_str(dir_t dir, char **val);

cs_error registry_readlocal_bool(dir_t dir, bool *val);

cs_error registry_readlocal_dir(dir_t dir, dir_t *val);

#endif