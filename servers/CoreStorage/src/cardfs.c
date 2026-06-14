/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * cardfs -- a minimal, kvs-inspired persistent OBJECT STORE on a CoreStorage
 * block device. This is an EXPLORATION of the relational/object filesystem
 * direction (notes/servers/CoreStorage/filesystem-direction.md), NOT the final
 * design: it is a flat key -> object map (the simplest expression of the
 * "objects with keys" model) so the on-disk persistence path can be exercised
 * end-to-end on real hardware before the real log-structured/COW design lands.
 *
 * On-disk layout (block_size B):
 *   LBA 0                 : superblock
 *   LBA 1 .. table_blocks : object table (array of 128-byte entries)
 *   LBA data_start ..     : object data blocks (bump-allocated)
 *
 * A TEMP self-test (started from CoreStorage module_init) waits for a block
 * device, formats it, puts a couple of objects, and reads them back.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "CoreStorage/storage.h"
#include "SysTaskMgr/task.h"

#define CARDFS_MAGIC "CARDFS01"
#define CARDFS_TABLE_BLOCKS 8
#define CARDFS_KEY_LEN 64
#define CARDFS_ENTRY_SIZE 128

typedef struct {
    char magic[8];
    uint32_t block_size;
    uint32_t table_blocks;
    uint64_t table_lba;
    uint64_t data_start_lba;
    uint64_t next_free_lba;
    uint64_t next_obj_id;
    uint64_t obj_count;
} PACKED cardfs_super_t;

typedef struct {
    uint64_t id;
    char key[CARDFS_KEY_LEN];
    uint64_t data_lba;
    uint64_t size;
    uint32_t valid;
    uint8_t pad[CARDFS_ENTRY_SIZE - 8 - CARDFS_KEY_LEN - 8 - 8 - 4];
} PACKED cardfs_obj_t;

static int blk_read(void *bdev, uint64_t lba, void *buf) {
    return storage_blockdev_read(bdev, lba, 1, buf);
}
static int blk_write(void *bdev, uint64_t lba, const void *buf) {
    return storage_blockdev_write(bdev, lba, 1, buf);
}

static uint32_t fs_bs(void *bdev) {
    const storage_blockdev_t *info = storage_blockdev_info(bdev);
    return info ? info->block_size : 512;
}

static int cardfs_format(void *bdev) {
    uint32_t bs = fs_bs(bdev);
    uint8_t *blk = (uint8_t *)malloc(bs);
    if (blk == NULL)
        return -1;

    // Zero the object table.
    memset(blk, 0, bs);
    for (uint32_t i = 0; i < CARDFS_TABLE_BLOCKS; i++)
        blk_write(bdev, 1 + i, blk);

    // Superblock.
    memset(blk, 0, bs);
    cardfs_super_t *sb = (cardfs_super_t *)blk;
    memcpy(sb->magic, CARDFS_MAGIC, 8);
    sb->block_size = bs;
    sb->table_blocks = CARDFS_TABLE_BLOCKS;
    sb->table_lba = 1;
    sb->data_start_lba = 1 + CARDFS_TABLE_BLOCKS;
    sb->next_free_lba = sb->data_start_lba;
    sb->next_obj_id = 1;
    sb->obj_count = 0;
    blk_write(bdev, 0, blk);

    free(blk);
    return 0;
}

static int read_super(void *bdev, cardfs_super_t *sb) {
    uint32_t bs = fs_bs(bdev);
    uint8_t *blk = (uint8_t *)malloc(bs);
    if (blk == NULL)
        return -1;
    int r = blk_read(bdev, 0, blk);
    if (r == 0) {
        memcpy(sb, blk, sizeof(*sb));
        if (memcmp(sb->magic, CARDFS_MAGIC, 8) != 0)
            r = -1;
    }
    free(blk);
    return r;
}

// Store `data` (len bytes) under `key`. Bump-allocates data blocks.
static int cardfs_put(void *bdev, const char *key, const void *data, uint32_t len) {
    cardfs_super_t sb;
    if (read_super(bdev, &sb) != 0)
        return -1;
    uint32_t bs = sb.block_size;
    uint32_t need = (len + bs - 1) / bs;
    if (need == 0)
        need = 1;

    uint64_t data_lba = sb.next_free_lba;
    uint8_t *blk = (uint8_t *)malloc(bs);
    if (blk == NULL)
        return -1;
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < need; i++) {
        memset(blk, 0, bs);
        uint32_t chunk = (len - i * bs) > bs ? bs : (len - i * bs);
        if ((int)(len - i * bs) > 0)
            memcpy(blk, src + i * bs, chunk);
        blk_write(bdev, data_lba + i, blk);
    }

    // Find a free table slot.
    int per_blk = bs / CARDFS_ENTRY_SIZE;
    int placed = 0;
    for (uint32_t tb = 0; tb < sb.table_blocks && !placed; tb++) {
        blk_read(bdev, sb.table_lba + tb, blk);
        for (int e = 0; e < per_blk; e++) {
            cardfs_obj_t *obj = (cardfs_obj_t *)(blk + e * CARDFS_ENTRY_SIZE);
            if (!obj->valid) {
                memset(obj, 0, CARDFS_ENTRY_SIZE);
                obj->id = sb.next_obj_id++;
                strncpy(obj->key, key, CARDFS_KEY_LEN - 1);
                obj->data_lba = data_lba;
                obj->size = len;
                obj->valid = 1;
                blk_write(bdev, sb.table_lba + tb, blk);
                placed = 1;
                break;
            }
        }
    }
    free(blk);
    if (!placed)
        return -1;

    sb.next_free_lba += need;
    sb.obj_count++;
    uint8_t *sblk = (uint8_t *)malloc(bs);
    if (sblk == NULL)
        return -1;
    memset(sblk, 0, bs);
    memcpy(sblk, &sb, sizeof(sb));
    blk_write(bdev, 0, sblk);
    free(sblk);
    return 0;
}

// Look up `key`; copies up to `maxlen` bytes into buf, writes the true size.
static int cardfs_get(void *bdev, const char *key, void *buf, uint32_t maxlen, uint32_t *out_size) {
    cardfs_super_t sb;
    if (read_super(bdev, &sb) != 0)
        return -1;
    uint32_t bs = sb.block_size;
    uint8_t *blk = (uint8_t *)malloc(bs);
    if (blk == NULL)
        return -1;

    int per_blk = bs / CARDFS_ENTRY_SIZE;
    cardfs_obj_t found;
    int hit = 0;
    for (uint32_t tb = 0; tb < sb.table_blocks && !hit; tb++) {
        blk_read(bdev, sb.table_lba + tb, blk);
        for (int e = 0; e < per_blk; e++) {
            cardfs_obj_t *obj = (cardfs_obj_t *)(blk + e * CARDFS_ENTRY_SIZE);
            if (obj->valid && strncmp(obj->key, key, CARDFS_KEY_LEN) == 0) {
                found = *obj;
                hit = 1;
                break;
            }
        }
    }
    if (!hit) {
        free(blk);
        return -1;
    }

    if (out_size)
        *out_size = (uint32_t)found.size;
    uint32_t copied = 0;
    uint32_t need = ((uint32_t)found.size + bs - 1) / bs;
    for (uint32_t i = 0; i < need; i++) {
        blk_read(bdev, found.data_lba + i, blk);
        uint32_t remain = (uint32_t)found.size - i * bs;
        uint32_t chunk = remain > bs ? bs : remain;
        if (copied + chunk > maxlen)
            chunk = (copied < maxlen) ? (maxlen - copied) : 0;
        if (chunk > 0)
            memcpy((uint8_t *)buf + copied, blk, chunk);
        copied += chunk;
    }
    free(blk);
    return 0;
}

// ---- TEMP self-test ----
static void cardfs_test_task(void *arg) {
    arg = NULL;
    // Wait for a block device to be registered (usb_storage enumerates late).
    void *bdev = NULL;
    while (bdev == NULL) {
        if (storage_blockdev_count() > 0)
            bdev = storage_blockdev_get(0);
        task_yield();
    }
    // A few extra yields so the device's self-test (if any) settles.
    for (int i = 0; i < 1000; i++)
        task_yield();

    DEBUG_PRINT("[cardfs] formatting block device 0\r\n");
    if (cardfs_format(bdev) != 0) {
        DEBUG_PRINT("[cardfs] format failed\r\n");
        return;
    }

    cardfs_put(bdev, "greeting", "hello cardinal", 14);
    cardfs_put(bdev, "answer", "forty-two", 9);

    char out[64];
    uint32_t sz = 0;
    memset(out, 0, sizeof(out));
    if (cardfs_get(bdev, "greeting", out, sizeof(out) - 1, &sz) == 0) {
        out[sz < sizeof(out) ? sz : sizeof(out) - 1] = 0;
        DEBUG_PRINT("[cardfs] get greeting -> '");
        DEBUG_PRINT(out);
        DEBUG_PRINT("'\r\n");
    } else {
        DEBUG_PRINT("[cardfs] get greeting failed\r\n");
    }

    memset(out, 0, sizeof(out));
    if (cardfs_get(bdev, "answer", out, sizeof(out) - 1, &sz) == 0) {
        out[sz < sizeof(out) ? sz : sizeof(out) - 1] = 0;
        DEBUG_PRINT("[cardfs] get answer -> '");
        DEBUG_PRINT(out);
        DEBUG_PRINT("'\r\n");
    }

    DEBUG_PRINT("[cardfs] self-test done\r\n");
    while (true)
        task_yield();
}

void cardfs_start_selftest(void) {
    cs_id task = 0;
    if (create_task_kernel("cardfs_test", task_permissions_kernel, &task) == CS_OK)
        start_task_kernel(task, cardfs_test_task, NULL);
}
