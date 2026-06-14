/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * CoreStorage: registry of block devices (with simple block I/O dispatch) and of
 * filesystem providers. A driver registers a block device; a filesystem (e.g.
 * cardfs) registers a provider and is offered each device via its probe. This is
 * the foundation userspace file I/O will sit on (still TODO -- see
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

static list_t fsprovider_list;
static int fsprovider_lock = 0;

// Offer one block device to each registered fs provider; first to claim wins.
// Called outside blockdev_lock so providers can do block I/O while probing.
static void probe_device_against_providers(storage_blockdev_t *dev) {
    local_spinlock_lock(&fsprovider_lock);
    int n = (int)list_len(&fsprovider_list);
    local_spinlock_unlock(&fsprovider_lock);
    for (int i = 0; i < n; i++) {
        local_spinlock_lock(&fsprovider_lock);
        storage_fsprovider_t *p =
            (i < (int)list_len(&fsprovider_list)) ? list_at(&fsprovider_list, (uint64_t)i) : NULL;
        local_spinlock_unlock(&fsprovider_lock);
        if (p != NULL && p->probe != NULL && p->probe(dev) == 0)
            break;
    }
}

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

    // Let any already-registered filesystem provider mount it.
    probe_device_against_providers(dev);
    return 0;
}

int storage_register_fsprovider(storage_fsprovider_t *desc) {
    storage_fsprovider_t *p = (storage_fsprovider_t *)malloc(sizeof(storage_fsprovider_t));
    *p = *desc;

    local_spinlock_lock(&fsprovider_lock);
    list_append(&fsprovider_list, p);
    local_spinlock_unlock(&fsprovider_lock);

    DEBUG_PRINT("[CoreStorage] Registered fs provider: ");
    DEBUG_PRINT(p->name);
    DEBUG_PRINT("\r\n");

    // Offer it every block device already present.
    local_spinlock_lock(&blockdev_lock);
    int n = (int)list_len(&blockdev_list);
    local_spinlock_unlock(&blockdev_lock);
    for (int i = 0; i < n; i++) {
        local_spinlock_lock(&blockdev_lock);
        storage_blockdev_t *dev =
            (i < (int)list_len(&blockdev_list)) ? list_at(&blockdev_list, (uint64_t)i) : NULL;
        local_spinlock_unlock(&blockdev_lock);
        if (dev != NULL && p->probe != NULL)
            p->probe(dev);
    }
    return 0;
}

int storage_unregister_blockdev(void *handle) {
    if (handle == NULL)
        return -1;
    local_spinlock_lock(&blockdev_lock);
    int n = (int)list_len(&blockdev_list);
    int found = -1;
    for (int i = 0; i < n; i++)
        if (list_at(&blockdev_list, (uint64_t)i) == handle) {
            found = i;
            break;
        }
    if (found >= 0)
        list_remove(&blockdev_list, (uint64_t)found);
    local_spinlock_unlock(&blockdev_lock);
    if (found < 0)
        return -1;

    DEBUG_PRINT("[CoreStorage] Unregistered block device: ");
    DEBUG_PRINT(((storage_blockdev_t *)handle)->name);
    DEBUG_PRINT("\r\n");
    free(handle);
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
    list_init(&fsprovider_list);
    DEBUG_PRINT("[CoreStorage] block device + fs-provider registry up\r\n");
    return 0;
}
