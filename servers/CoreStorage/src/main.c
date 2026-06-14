/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * CoreStorage: registry of block devices, with simple block I/O dispatch. This
 * is the foundation a filesystem provider sits on. Filesystem-provider
 * registration and userspace file I/O are still TODO (see
 * notes/servers/CoreStorage/filesystem-direction.md).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdlist.h>
#include <cardinal/local_spinlock.h>

#include "CoreStorage/storage.h"

static list_t blockdev_list;
static int blockdev_lock = 0;

int storage_register_blockdev(storage_blockdev_t *desc, void **handle) {
    storage_blockdev_t *dev = (storage_blockdev_t *)malloc(sizeof(storage_blockdev_t));
    *dev = *desc;

    local_spinlock_lock(&blockdev_lock);
    list_append(&blockdev_list, dev);
    local_spinlock_unlock(&blockdev_lock);

    if (handle)
        *handle = dev;

    DEBUG_PRINT("[CoreStorage] Registered block device: ");
    DEBUG_PRINT(dev->name);
    DEBUG_PRINT("\r\n");
    return 0;
}

int storage_blockdev_count(void) {
    local_spinlock_lock(&blockdev_lock);
    int n = (int)list_len(&blockdev_list);
    local_spinlock_unlock(&blockdev_lock);
    return n;
}

void *storage_blockdev_get(int idx) {
    local_spinlock_lock(&blockdev_lock);
    void *r = NULL;
    if (idx >= 0 && idx < (int)list_len(&blockdev_list))
        r = list_at(&blockdev_list, (uint64_t)idx);
    local_spinlock_unlock(&blockdev_lock);
    return r;
}

const storage_blockdev_t *storage_blockdev_info(void *handle) {
    return (const storage_blockdev_t *)handle;
}

int storage_blockdev_read(void *handle, uint64_t lba, uint32_t count, void *buf) {
    storage_blockdev_t *dev = (storage_blockdev_t *)handle;
    if (dev == NULL || dev->read == NULL)
        return -1;
    if (lba + count > dev->block_count)
        return -1;
    return dev->read(dev->state, lba, count, buf);
}

int storage_blockdev_write(void *handle, uint64_t lba, uint32_t count, const void *buf) {
    storage_blockdev_t *dev = (storage_blockdev_t *)handle;
    if (dev == NULL || dev->write == NULL)
        return -1;
    if (lba + count > dev->block_count)
        return -1;
    return dev->write(dev->state, lba, count, buf);
}

int module_init() {
    list_init(&blockdev_list);
    DEBUG_PRINT("[CoreStorage] block device registry up\r\n");
    return 0;
}
