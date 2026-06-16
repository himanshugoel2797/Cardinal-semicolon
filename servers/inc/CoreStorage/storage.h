// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORESTORAGE_H
#define CARDINALSEMI_CORESTORAGE_H

#include <stdint.h>

// A registered block device. A driver fills this in and registers it; CoreStorage
// exposes block read/write to filesystem providers and (eventually) userspace.
//
// NOTE: first-cut interface, expected to be reviewed/redesigned alongside the
// filesystem direction (see notes/servers/CoreStorage/filesystem-direction.md).
typedef struct {
    char name[256];
    void *state;
    uint32_t block_size;   // bytes per block (e.g. 512)
    uint64_t block_count;  // number of blocks

    // Read/write `count` blocks starting at `lba`. Return 0 on success, <0 on
    // error. May be called concurrently from different callers; the driver is
    // responsible for its own serialisation.
    int (*read)(void *state, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *state, uint64_t lba, uint32_t count, const void *buf);

    // CoreStorage-internal (drivers must not set this): becomes non-zero once a
    // filesystem provider has claimed/mounted this device, so it is never offered
    // to a second provider. Zeroed on registration.
    int claimed;
} storage_blockdev_t;

int storage_register_blockdev(storage_blockdev_t *desc, void **handle);

// Remove a previously-registered block device (e.g. on USB unplug). `handle` is
// what storage_register_blockdev wrote. Returns 0 on success, <0 if not found.
// NOTE: any filesystem provider that mounted this device is not yet notified --
// providers gain an unmount hook when the FS-provider API grows one.
int storage_unregister_blockdev(void *handle);

// Enumerate / address registered block devices by index.
int storage_blockdev_count(void);
void *storage_blockdev_get(int idx);
const storage_blockdev_t *storage_blockdev_info(void *handle);

// Block I/O against a registered device handle.
int storage_blockdev_read(void *handle, uint64_t lba, uint32_t count, void *buf);
int storage_blockdev_write(void *handle, uint64_t lba, uint32_t count, const void *buf);

// A filesystem provider (e.g. cardfs, tarfs) plugs into CoreStorage. Each
// registered provider is offered every block device -- those already present at
// registration time, and each new one as it registers -- via `probe`. `probe`
// is a read-only "is this volume mine?" check: it must NOT format or otherwise
// mutate a device it does not recognise. Return 0 to claim/mount the device, <0
// to decline. This is the synchronous replacement for a filesystem polling the
// block-device list; the probe runs in the registering driver's context.
//
// NOTE: first-cut interface, expected to evolve with the filesystem direction
// (see notes/servers/CoreStorage/filesystem-direction.md).
typedef struct {
    char name[32];
    int (*probe)(void *blockdev_handle);
} storage_fsprovider_t;

int storage_register_fsprovider(storage_fsprovider_t *desc);

#endif
