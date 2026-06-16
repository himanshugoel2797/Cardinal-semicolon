/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * USB Mass Storage (Bulk-Only Transport + SCSI) driver. Registers as a CoreUsb
 * class driver for bInterfaceClass==Mass Storage; on probe it finds the bulk
 * IN/OUT endpoints and runs INQUIRY / READ CAPACITY / READ(10) to prove the
 * path works.
 *
 * Test interface (intentionally simple, to be reviewed/redesigned): results are
 * printed to the debug console and a read of LBA 0 is dumped. A real driver
 * would register a block device with CoreStorage -- see
 * notes/servers/CoreStorage/filesystem-direction.md and CoreUsb-roadmap.md.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "CoreUsb/usb.h"
#include "CoreStorage/storage.h"
#include "SysTaskMgr/task.h"

#define CBW_SIGNATURE 0x43425355u  // "USBC"
#define CSW_SIGNATURE 0x53425355u  // "USBS"

typedef struct {
    usb_enum_device_t *dev;
    int in_ep, out_ep;
    int in_mps, out_mps;
    uint32_t tag;
    bool in_use;
    cs_id task;
    volatile bool stop;     // remove() requests the task to exit
    volatile bool stopped;  // task acknowledges it has exited
    uint32_t block_size;
    uint64_t block_count;
    int io_lock;  // serialise block I/O (BBB is one command at a time)
    void *storage_handle;  // non-NULL once registered with CoreStorage
} stor_dev_t;

#define MAX_STOR 4
static stor_dev_t stor_devs[MAX_STOR];

static int usb_storage_read(void *state, uint64_t lba, uint32_t count, void *buf);
static int usb_storage_write(void *state, uint64_t lba, uint32_t count, const void *buf);

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// One Bulk-Only command: CBW (out) -> optional data -> CSW (in).
// Returns the CSW status (0 = good) or <0 on transport failure.
static int bbb_command(stor_dev_t *s, const uint8_t *cmd, int cmdlen,
                       void *data, int datalen, int dir_in) {
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    *(uint32_t *)&cbw[0] = CBW_SIGNATURE;
    *(uint32_t *)&cbw[4] = ++s->tag;
    *(uint32_t *)&cbw[8] = (uint32_t)datalen;
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;  // LUN 0
    cbw[14] = (uint8_t)cmdlen;
    memcpy(&cbw[15], cmd, cmdlen);

    if (usb_dev_bulk(s->dev, s->out_ep, s->out_mps, cbw, sizeof(cbw), 0) != (int)sizeof(cbw))
        return -1;

    if (datalen > 0) {
        int ep = dir_in ? s->in_ep : s->out_ep;
        int mps = dir_in ? s->in_mps : s->out_mps;
        int r = usb_dev_bulk(s->dev, ep, mps, data, datalen, dir_in);
        if (r < 0)
            return -1;
    }

    uint8_t csw[13];
    memset(csw, 0, sizeof(csw));
    if (usb_dev_bulk(s->dev, s->in_ep, s->in_mps, csw, sizeof(csw), 1) != (int)sizeof(csw))
        return -1;
    if (*(uint32_t *)&csw[0] != CSW_SIGNATURE)
        return -1;
    return csw[12];  // bCSWStatus
}

static void dump_hex(const uint8_t *p, int n) {
    char b[4];
    for (int i = 0; i < n; i++) {
        DEBUG_PRINT(itoa(p[i], b, 16));
        DEBUG_PRINT(" ");
    }
    DEBUG_PRINT("\r\n");
}

static void stor_test_task(void *arg) {
    stor_dev_t *s = (stor_dev_t *)arg;
    uint8_t cmd[16];
    uint8_t buf[512];

    // INQUIRY (36 bytes): print the vendor/product identification.
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x12;  // INQUIRY
    cmd[4] = 36;
    memset(buf, 0, sizeof(buf));
    if (bbb_command(s, cmd, 6, buf, 36, 1) == 0) {
        char id[29];
        memcpy(id, &buf[8], 28);  // vendor(8)+product(16)+rev(4)
        id[28] = 0;
        DEBUG_PRINT("[usb_storage] INQUIRY id: ");
        DEBUG_PRINT(id);
        DEBUG_PRINT("\r\n");
    } else {
        DEBUG_PRINT("[usb_storage] INQUIRY failed\r\n");
    }

    // READ CAPACITY(10): last LBA + block size.
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x25;
    memset(buf, 0, sizeof(buf));
    if (bbb_command(s, cmd, 10, buf, 8, 1) == 0) {
        uint32_t last_lba = get_be32(&buf[0]);
        uint32_t blk = get_be32(&buf[4]);
        s->block_size = blk ? blk : 512;
        s->block_count = (uint64_t)last_lba + 1;
        char b[12];
        DEBUG_PRINT("[usb_storage] capacity: last_lba=");
        DEBUG_PRINT(itoa((int)last_lba, b, 10));
        DEBUG_PRINT(" block_size=");
        DEBUG_PRINT(itoa((int)blk, b, 10));
        DEBUG_PRINT("\r\n");
    } else {
        DEBUG_PRINT("[usb_storage] READ CAPACITY failed\r\n");
        s->block_size = 512;
        s->block_count = 0;
    }

    // A device with no usable capacity must not be registered: CoreStorage would
    // reject every I/O against it, and consumers (e.g. cardfs) would pick it up
    // as block device 0 and fail opaquely. Bail out instead.
    if (s->block_count == 0) {
        DEBUG_PRINT("[usb_storage] no usable capacity; not registering block device\r\n");
        goto park;  // stay alive (stop-aware) so remove() can reclaim cleanly
    }

    // Register as a CoreStorage block device.
    storage_blockdev_t bd;
    memset(&bd, 0, sizeof(bd));
    strncpy(bd.name, "USB Mass Storage", 32);
    bd.state = s;
    bd.block_size = s->block_size;
    bd.block_count = s->block_count;
    bd.read = usb_storage_read;
    bd.write = usb_storage_write;
    storage_register_blockdev(&bd, &s->storage_handle);

    // Verify the block path end-to-end: read LBA 0 back through CoreStorage.
    memset(buf, 0, sizeof(buf));
    if (storage_blockdev_read(s->storage_handle, 0, 1, buf) == 0) {
        DEBUG_PRINT("[usb_storage] via CoreStorage, LBA0 first 16 bytes: ");
        dump_hex(buf, 16);
    } else {
        DEBUG_PRINT("[usb_storage] CoreStorage read failed\r\n");
    }

    DEBUG_PRINT("[usb_storage] self-test done\r\n");

park:
    while (!s->stop)
        task_yield();
    s->stopped = true;  // hand off: remove() may now reclaim the device
    task_end_kernel(task_current());
}

// CoreStorage block read/write callbacks. SCSI READ(10)/WRITE(10), chunked to
// the per-transfer data limit (BBB data stage <= ~2 KiB), serialised by io_lock
// so a multi-block (multi-command) op stays atomic.
#define STOR_MAX_BLOCKS_PER_CMD 4  // 4 * 512 = 2048

static int usb_storage_read(void *state, uint64_t lba, uint32_t count, void *buf) {
    stor_dev_t *s = (stor_dev_t *)state;
    uint8_t *out = (uint8_t *)buf;
    int ret = 0;
    local_spinlock_lock(&s->io_lock);
    while (count > 0) {
        uint32_t chunk = count > STOR_MAX_BLOCKS_PER_CMD ? STOR_MAX_BLOCKS_PER_CMD : count;
        uint8_t cmd[16];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x28;  // READ(10)
        put_be32(&cmd[2], (uint32_t)lba);
        cmd[7] = (uint8_t)(chunk >> 8);
        cmd[8] = (uint8_t)(chunk & 0xff);
        if (bbb_command(s, cmd, 10, out, (int)(chunk * s->block_size), 1) != 0) {
            ret = -1;
            break;
        }
        lba += chunk;
        count -= chunk;
        out += chunk * s->block_size;
    }
    local_spinlock_unlock(&s->io_lock);
    return ret;
}

static int usb_storage_write(void *state, uint64_t lba, uint32_t count, const void *buf) {
    stor_dev_t *s = (stor_dev_t *)state;
    const uint8_t *in = (const uint8_t *)buf;
    int ret = 0;
    local_spinlock_lock(&s->io_lock);
    while (count > 0) {
        uint32_t chunk = count > STOR_MAX_BLOCKS_PER_CMD ? STOR_MAX_BLOCKS_PER_CMD : count;
        uint8_t cmd[16];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x2A;  // WRITE(10)
        put_be32(&cmd[2], (uint32_t)lba);
        cmd[7] = (uint8_t)(chunk >> 8);
        cmd[8] = (uint8_t)(chunk & 0xff);
        if (bbb_command(s, cmd, 10, (void *)in, (int)(chunk * s->block_size), 0) != 0) {
            ret = -1;
            break;
        }
        lba += chunk;
        count -= chunk;
        in += chunk * s->block_size;
    }
    local_spinlock_unlock(&s->io_lock);
    return ret;
}

static int stor_probe(usb_enum_device_t *dev) {
    int in_mps = 0, out_mps = 0;
    int in_ep = usb_dev_find_endpoint(dev, 2 /*bulk*/, 1 /*IN*/, &in_mps);
    int out_ep = usb_dev_find_endpoint(dev, 2 /*bulk*/, 0 /*OUT*/, &out_mps);
    if (in_ep < 0 || out_ep < 0) {
        DEBUG_PRINT("[usb_storage] missing bulk endpoints; not claiming\r\n");
        return -1;
    }

    stor_dev_t *s = NULL;
    for (int i = 0; i < MAX_STOR; i++)
        if (!stor_devs[i].in_use) {
            s = &stor_devs[i];
            break;
        }
    if (s == NULL)
        return -1;

    s->dev = dev;
    s->in_ep = in_ep;
    s->out_ep = out_ep;
    s->in_mps = in_mps > 0 ? in_mps : 64;
    s->out_mps = out_mps > 0 ? out_mps : 64;
    s->tag = 0;
    s->stop = false;
    s->stopped = false;
    s->storage_handle = NULL;
    s->in_use = true;

    DEBUG_PRINT("[usb_storage] claimed mass-storage device\r\n");

    s->task = 0;
    if (task_create_kernel("usb_storage_test", task_permissions_kernel, &s->task) != CS_OK) {
        s->in_use = false;
        return -1;
    }
    task_start_kernel(s->task, stor_test_task, s);
    return 0;
}

static void stor_remove(usb_enum_device_t *dev) {
    for (int i = 0; i < MAX_STOR; i++) {
        stor_dev_t *s = &stor_devs[i];
        if (!s->in_use || s->dev != dev)
            continue;
        // Stop the task and wait for it to exit before reclaiming the device.
        s->stop = true;
        while (!s->stopped)
            task_yield();
        if (s->storage_handle != NULL) {
            storage_unregister_blockdev(s->storage_handle);
            s->storage_handle = NULL;
        }
        s->in_use = false;
        DEBUG_PRINT("[usb_storage] device removed\r\n");
    }
}

int module_init() {
    memset(stor_devs, 0, sizeof(stor_devs));
    usb_register_class_driver(USB_CLASS_MASS_STORAGE, stor_probe, stor_remove);
    DEBUG_PRINT("[usb_storage] registered mass-storage class driver\r\n");
    return 0;
}
