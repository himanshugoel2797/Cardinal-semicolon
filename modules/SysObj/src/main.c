/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include "kvs.h"
#include "obj.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

static kvs_t *kern_registry;
static int kern_lock = 0;

static int obj_getkvs(const char *path, kvs_t **k NONNULL)
{
    return kvs_walk_path(kern_registry, &kern_lock, MAX_OBJ_KEYLEN, path, k);
}

cs_error obj_createdirectory(const char *path, const char *dirname)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *n_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (dirname == NULL)
        return CS_INVALIDARG;

    // Get the parent kvs
    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    DEBUG_PRINT("[SysReg] CreateDirectory: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(dirname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);

    // Check if the requested directory already exists
    err = kvs_find(parent_kvs, dirname, NULL);
    if (err == kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_EXISTS;
    }

    // Create and add the new directory
    err = kvs_create(&n_kvs);
    if (err != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    err = kvs_add_child(parent_kvs, dirname, n_kvs);
    if (err != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_addkey_uint(const char *path, const char *keyname, uint64_t val)
{
    kvs_t *parent_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    DEBUG_PRINT("[SysReg] AddKeyUInt: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(keyname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);
    err = kvs_add_uint(parent_kvs, keyname, val);
    local_spinlock_unlock(&kern_lock);
    if (err != kvs_ok)
        return CS_FAILURE;

    return CS_OK;
}

cs_error obj_addkey_ptr(const char *path, const char *keyname, uintptr_t val)
{
    kvs_t *parent_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    DEBUG_PRINT("[SysReg] AddKeyPtr: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(keyname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);
    err = kvs_add_ptr(parent_kvs, keyname, (void *)val);
    local_spinlock_unlock(&kern_lock);
    if (err != kvs_ok)
        return CS_FAILURE;

    return CS_OK;
}

cs_error obj_addkey_int(const char *path, const char *keyname, int64_t val)
{
    kvs_t *parent_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    DEBUG_PRINT("[SysReg] AddKeyInt: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(keyname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);
    err = kvs_add_sint(parent_kvs, keyname, val);
    local_spinlock_unlock(&kern_lock);
    if (err != kvs_ok)
        return CS_FAILURE;

    return CS_OK;
}

cs_error obj_addkey_str(const char *path, const char *keyname,
                        const char *val)
{
    kvs_t *parent_kvs = NULL;
    char *strstore = NULL;
    size_t storelen = 0;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    storelen = MIN((size_t)MAX_OBJ_STRLEN, strlen(val));
    strstore = malloc(storelen);
    if (strstore == NULL)
        return CS_FAILURE;

    strncpy(strstore, val, storelen);

    DEBUG_PRINT("[SysReg] AddKeyStr: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(keyname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);
    err = kvs_add_str(parent_kvs, keyname, strstore);
    local_spinlock_unlock(&kern_lock);
    if (err != kvs_ok)
        return CS_FAILURE;

    return CS_OK;
}

cs_error obj_addkey_bool(const char *path, const char *keyname, bool val)
{
    kvs_t *parent_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    DEBUG_PRINT("[SysReg] AddKeyBool: ");
    DEBUG_PRINT(path);
    DEBUG_PRINT("/");
    DEBUG_PRINT(keyname);
    DEBUG_PRINT("\r\n");

    local_spinlock_lock(&kern_lock);
    err = kvs_add_bool(parent_kvs, keyname, val);
    local_spinlock_unlock(&kern_lock);
    if (err != kvs_ok)
        return CS_FAILURE;

    return CS_OK;
}

cs_error obj_readkey_uint(const char *path, const char *keyname,
                          uint64_t *val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_uint)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (val != NULL && kvs_get_uint(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_readkey_ptr(const char *path, const char *keyname,
                         uintptr_t *val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_ptr)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (val != NULL && kvs_get_ptr(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_readkey_int(const char *path, const char *keyname, int64_t *val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_sint)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (val != NULL && kvs_get_sint(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_readkey_str(const char *path, const char *keyname, char *val,
                         size_t *val_len)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;
    char *strval = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_str)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (val != NULL && kvs_get_str(key_kvs, &strval) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (val != NULL && val_len != NULL)
    {
        strncpy(val, strval, *val_len);
        *val_len = strlen(strval);
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_readkey_bool(const char *path, const char *keyname, bool *val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_bool)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (val != NULL && kvs_get_bool(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_writekey_uint(const char *path, const char *keyname,
                          uint64_t val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_uint)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (kvs_set_uint(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_writekey_ptr(const char *path, const char *keyname,
                         uintptr_t val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_ptr)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (kvs_set_ptr(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_writekey_int(const char *path, const char *keyname, int64_t val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_sint)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (kvs_set_sint(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_writekey_str(const char *path, const char *keyname, const char *val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;
        
    if (val == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_str)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    // kvs_set_str only stores the pointer (it never writes through it), so the
    // const cast is safe; val keeps its const contract to our callers.
    if (kvs_set_str(key_kvs, (char *)val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_writekey_bool(const char *path, const char *keyname, bool val)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;
    kvs_val_type valtype = kvs_val_uninit;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (kvs_get_type(key_kvs, &valtype) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }

    if (valtype != kvs_val_bool)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_TYPEMISMATCH;
    }

    if (kvs_set_bool(key_kvs, val) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_FAILURE;
    }
    local_spinlock_unlock(&kern_lock);
    return CS_OK;
}

cs_error obj_removekey(const char *path, const char *keyname)
{
    kvs_t *parent_kvs = NULL;
    kvs_t *key_kvs = NULL;

    if (path == NULL)
        return CS_INVALIDARG;

    if (keyname == NULL)
        return CS_INVALIDARG;

    int err = obj_getkvs(path, &parent_kvs);
    if (err != CS_OK)
        return err;

    local_spinlock_lock(&kern_lock);
    if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    {
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    bool locked = false;
    if (kvs_islocked(key_kvs, &locked) != kvs_ok){
        local_spinlock_unlock(&kern_lock);
        return CS_DNE;
    }

    if (!locked)
        kvs_remove(parent_kvs, key_kvs);
    local_spinlock_unlock(&kern_lock);

    return CS_OK;
}

cs_error obj_removedirectory(const char *path, const char *dirname)
{
    return obj_removekey(path, dirname);
}

cs_error obj_getdirectory(const char *path, dir_t *dir)
{
    return obj_getkvs(path, (kvs_t **)dir);
}

cs_error obj_next(dir_t *dir)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_next((kvs_t **)dir);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_key(dir_t dir, char *keyname)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_key((kvs_t *)dir, keyname);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_uint(dir_t dir, uint64_t *val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_uint((kvs_t *)dir, val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_bool(dir_t dir, bool *val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_bool((kvs_t *)dir, val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_ptr(dir_t dir, void **val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_ptr((kvs_t *)dir, (uintptr_t *)val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_int(dir_t dir, int64_t *val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_sint((kvs_t *)dir, val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_str(dir_t dir, char **val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_str((kvs_t *)dir, val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_readlocal_dir(dir_t dir, dir_t *val)
{
    local_spinlock_lock(&kern_lock);
    int err = kvs_get_child((kvs_t *)dir, (kvs_t **)val);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_lock(dir_t dir){
    local_spinlock_lock(&kern_lock);
    int err = kvs_lockentry((kvs_t *)dir);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_unlock(dir_t dir){
    local_spinlock_lock(&kern_lock);
    int err = kvs_unlockentry((kvs_t *)dir);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

cs_error obj_islocked(dir_t dir, bool *status){
    local_spinlock_lock(&kern_lock);
    int err = kvs_islocked((kvs_t *)dir, status);
    local_spinlock_unlock(&kern_lock);
    if (err != CS_OK)
        return CS_DNE;
    return CS_OK;
}

void sysobj_register_tests(void);

int module_init() {
    if (kvs_create(&kern_registry) != kvs_ok)
        PANIC("Failed to initialize object store.");
    sysobj_register_tests();
    return 0;
}