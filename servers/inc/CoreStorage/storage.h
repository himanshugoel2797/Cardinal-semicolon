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
} storage_blockdev_t;

int storage_register_blockdev(storage_blockdev_t *desc, void **handle);

// Enumerate / address registered block devices by index.
int storage_blockdev_count(void);
void *storage_blockdev_get(int idx);
const storage_blockdev_t *storage_blockdev_info(void *handle);

// Block I/O against a registered device handle.
int storage_blockdev_read(void *handle, uint64_t lba, uint32_t count, void *buf);
int storage_blockdev_write(void *handle, uint64_t lba, uint32_t count, const void *buf);

#endif
