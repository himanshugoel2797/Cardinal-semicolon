#include "common.h"
#include "kvs.h"
#include "priv_reg.h"
#include "registry.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

// Parse tables and store them in a registry
// Load and initialize second stage general purpose registry after memory module
// has been initialized.

// Determine the system's complete topology, add it to the registry
// Get the system's complete information, PCI, NUMA, ACPI etc
// Use this to initialize the entire system in one go

// First setup physical memory and virtual memory for the primary core, then
// install the local IDT and use it to boot all cores
// Start the other cores, have them obtain a lock on the registry and determine
// their topological parameters
// pass these parameters to initialize their physical and virtual memory
// managers
// BSP behaves like a core and follows the same procedure
// All cores enter scheduler
// Initialization task is executed, loading in drivers and starting services
// User mode handoff

// Registry is an in-memory database

static kvs_t *kern_registry;

static int registry_getkvs(const char *path, kvs_t **k NONNULL) {

  char kvs_key[key_len];
  const char *n_part = NULL;
  kvs_t *cur_kvs = kern_registry;

  if (strlen(path) == 0) {
    *k = cur_kvs;
    return registry_err_ok;
  }

  do {
    n_part = strchr(path, '/');
    if (n_part == NULL)
      n_part = strchr(path, 0);

    if (n_part - path > MAX_REGISTRY_KEYLEN)
      return registry_err_dne;

    memset(kvs_key, 0, key_len);
    strncpy(kvs_key, path, n_part - path);

    if (kvs_find(cur_kvs, kvs_key, &cur_kvs) != kvs_ok) {
      *k = cur_kvs;
      return registry_err_dne;
    }

    if (kvs_get_child(cur_kvs, cur_kvs, &cur_kvs) != kvs_ok) {
      PANIC("Unexpected error!");
    }

    *k = cur_kvs;
    path = n_part + 1;

  } while (*n_part != 0);

  return registry_err_ok;
}

int registry_createdirectory(const char *path, const char *dirname) {
  kvs_t *parent_kvs = NULL;
  kvs_t *n_kvs = NULL;

  if (path == NULL)
    return registry_err_invalidargs;

  // Get the parent kvs
  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  // Check if the requested directory already exists
  err = kvs_find(parent_kvs, dirname, NULL);
  if (err == kvs_ok)
    return registry_err_exists;

  // Create and add the new directory
  err = kvs_create(&n_kvs);
  if (err != kvs_ok)
    return registry_err_failure;

  err = kvs_add_child(parent_kvs, dirname, n_kvs);
  if (err != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_addkey_uint(const char *path, const char *keyname, uint64_t val) {
  kvs_t *parent_kvs = NULL;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  err = kvs_add_uint(parent_kvs, keyname, val);
  if (err != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_addkey_int(const char *path, const char *keyname, int64_t val) {
  kvs_t *parent_kvs = NULL;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  err = kvs_add_sint(parent_kvs, keyname, val);
  if (err != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_addkey_str(const char *path, const char *keyname,
                        const char *val) {
  kvs_t *parent_kvs = NULL;
  char *strstore = NULL;
  size_t storelen = 0;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  storelen = MIN((size_t)MAX_REGISTRY_STRLEN, strlen(val));
  strstore = malloc(storelen);
  if (strstore == NULL)
    return registry_err_failure;

  strncpy(strstore, val, storelen);

  err = kvs_add_str(parent_kvs, keyname, strstore);
  if (err != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_readkey_uint(const char *path, const char *keyname,
                          uint64_t *val) {
  kvs_t *parent_kvs = NULL;
  kvs_t *key_kvs = NULL;
  kvs_val_type valtype = kvs_val_uninit;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    return registry_err_dne;

  if (kvs_get_type(parent_kvs, key_kvs, &valtype) != kvs_ok)
    return registry_err_failure;

  if (valtype != kvs_val_uint)
    return registry_err_typematchfailure;

  if (val != NULL && kvs_get_uint(parent_kvs, key_kvs, val) != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_readkey_int(const char *path, const char *keyname, int64_t *val) {
  kvs_t *parent_kvs = NULL;
  kvs_t *key_kvs = NULL;
  kvs_val_type valtype = kvs_val_uninit;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    return registry_err_dne;

  if (kvs_get_type(parent_kvs, key_kvs, &valtype) != kvs_ok)
    return registry_err_failure;

  if (valtype != kvs_val_sint)
    return registry_err_typematchfailure;

  if (val != NULL && kvs_get_sint(parent_kvs, key_kvs, val) != kvs_ok)
    return registry_err_failure;

  return registry_err_ok;
}

int registry_readkey_str(const char *path, const char *keyname, char *val,
                         size_t *val_len) {
  kvs_t *parent_kvs = NULL;
  kvs_t *key_kvs = NULL;
  kvs_val_type valtype = kvs_val_uninit;
  char *strval = NULL;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    return registry_err_dne;

  if (kvs_get_type(parent_kvs, key_kvs, &valtype) != kvs_ok)
    return registry_err_failure;

  if (valtype != kvs_val_str)
    return registry_err_typematchfailure;

  if (val != NULL && kvs_get_str(parent_kvs, key_kvs, &strval) != kvs_ok)
    return registry_err_failure;

  if (val != NULL && val_len != NULL) {
    strncpy(val, strval, *val_len);
    *val_len = strlen(strval);
  }

  return registry_err_ok;
}

int registry_removekey(const char *path, const char *keyname) {

  kvs_t *parent_kvs = NULL;
  kvs_t *key_kvs = NULL;

  if (path == NULL)
    return registry_err_invalidargs;

  if (keyname == NULL)
    return registry_err_invalidargs;

  int err = registry_getkvs(path, &parent_kvs);
  if (err != registry_err_ok)
    return err;

  if (kvs_find(parent_kvs, keyname, &key_kvs) != kvs_ok)
    return registry_err_dne;

  kvs_remove(parent_kvs, key_kvs);

  return registry_err_ok;
}

int registry_removedirectory(const char *path, const char *dirname) {
  return registry_removekey(path, dirname);
}

#define REG_INIT_FAIL_STR "Failed to initialize registry."

int module_init() {
  if (kvs_create(&kern_registry) != kvs_ok)
    PANIC(REG_INIT_FAIL_STR);

  if (registry_createdirectory("", "HW") != registry_err_ok)
    PANIC(REG_INIT_FAIL_STR);

  if (registry_createdirectory("HW", "BOOTINFO") != registry_err_ok)
    PANIC(REG_INIT_FAIL_STR);

  if (registry_createdirectory("HW", "PHYS_MEM") != registry_err_ok)
    PANIC(REG_INIT_FAIL_STR);

  if (registry_createdirectory("HW", "VIRT_MEM") != registry_err_ok)
    PANIC(REG_INIT_FAIL_STR);

  // TODO: move the following directory into platform
  if (add_bootinfo() != 0)
    PANIC("Registry: HW/BOOTINFO initialization failure.");

  if (add_platform_info() != 0)
    PANIC("Registry: platform information initialization failure.");

  // TODO: eventually also load any registry info saved in the initrd,
  // contains info such as primary partition information

  return 0;
}