// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// SysTest suite for CoreStorage's block-device registry. Exercises register /
// info / read / write / unregister against a mock block device backed by a
// static buffer. All tests run inline (pure registry logic, never blocks).

#include <stdint.h>
#include <string.h>

#include "SysTest/test.h"
#include "CoreStorage/storage.h"

#define MOCK_BLOCK_SIZE 512u
#define MOCK_BLOCK_COUNT 2u
#define MOCK_BACKING_SIZE (MOCK_BLOCK_SIZE * MOCK_BLOCK_COUNT)  // 1024 bytes

static uint8_t mock_backing[MOCK_BACKING_SIZE];

static int mock_read(void *state, uint64_t lba, uint32_t count, void *buf) {
    (void)state;
    uint64_t off = lba * MOCK_BLOCK_SIZE;
    uint64_t len = (uint64_t)count * MOCK_BLOCK_SIZE;
    if (off + len > MOCK_BACKING_SIZE)
        return -1;
    memcpy(buf, mock_backing + off, len);
    return 0;
}

static int mock_write(void *state, uint64_t lba, uint32_t count, const void *buf) {
    (void)state;
    uint64_t off = lba * MOCK_BLOCK_SIZE;
    uint64_t len = (uint64_t)count * MOCK_BLOCK_SIZE;
    if (off + len > MOCK_BACKING_SIZE)
        return -1;
    memcpy(mock_backing + off, buf, len);
    return 0;
}

static void make_mock_desc(storage_blockdev_t *desc) {
    memset(desc, 0, sizeof(*desc));
    strncpy(desc->name, "mock0", sizeof(desc->name) - 1);
    desc->state = NULL;
    desc->block_size = MOCK_BLOCK_SIZE;
    desc->block_count = MOCK_BLOCK_COUNT;
    desc->read = mock_read;
    desc->write = mock_write;
}

// Register returns 0, hands back a non-NULL handle, and bumps the device count;
// unregister returns 0 and drops the count back.
static void test_register_unregister(test_ctx_t *ctx) {
    int before = storage_blockdev_count();

    storage_blockdev_t desc;
    make_mock_desc(&desc);

    void *handle = NULL;
    int rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, handle != NULL);
    TEST_CHECK_EQ_U(ctx, storage_blockdev_count(), before + 1);

    rc = storage_unregister_blockdev(handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_U(ctx, storage_blockdev_count(), before);
}

// The info view of a handle reflects the descriptor that was registered.
static void test_info_roundtrip(test_ctx_t *ctx) {
    storage_blockdev_t desc;
    make_mock_desc(&desc);

    void *handle = NULL;
    int rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, handle != NULL);

    const storage_blockdev_t *info = storage_blockdev_info(handle);
    TEST_CHECK(ctx, info != NULL);
    if (info != NULL) {
        TEST_CHECK(ctx, strcmp(info->name, "mock0") == 0);
        TEST_CHECK_EQ_U(ctx, info->block_size, MOCK_BLOCK_SIZE);
        TEST_CHECK_EQ_U(ctx, info->block_count, MOCK_BLOCK_COUNT);
    }

    storage_unregister_blockdev(handle);
}

// A write through the handle lands in the backing buffer and reads back byte
// for byte. The test also verifies that the unwritten block (LBA 0) is
// untouched, catching broken offset/length in the production dispatch.
static void test_read_write_roundtrip(test_ctx_t *ctx) {
    // Zero the backing buffer explicitly so the untouched-region check is
    // reliable even if a previous test run left debris.
    memset(mock_backing, 0, sizeof(mock_backing));

    storage_blockdev_t desc;
    make_mock_desc(&desc);

    void *handle = NULL;
    int rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    if (handle == NULL) {
        TEST_FAIL(ctx, "register returned NULL handle");
        return;
    }

    // Non-trivial sequential pattern: byte i gets value (i * 3 + 0x5A) & 0xFF.
    // This catches copy-by-word bugs that would pass an all-zeros or all-same
    // pattern, while staying in the 0x00-0xFF range for easy visual inspection.
    uint8_t out[MOCK_BLOCK_SIZE];
    for (uint32_t i = 0; i < MOCK_BLOCK_SIZE; i++)
        out[i] = (uint8_t)((i * 3u + 0x5Au) & 0xFFu);

    // Write to LBA 1 (non-zero offset); LBA 0 must remain untouched.
    rc = storage_blockdev_write(handle, 1, 1, out);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // Read back LBA 1 and confirm byte-exact match.
    uint8_t in[MOCK_BLOCK_SIZE];
    memset(in, 0xCC, sizeof(in));
    rc = storage_blockdev_read(handle, 1, 1, in);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, memcmp(out, in, MOCK_BLOCK_SIZE) == 0,
                   "read-back of LBA 1 does not match written data");

    // LBA 0 must still be all-zeros — a broken offset would clobber it.
    uint8_t lba0[MOCK_BLOCK_SIZE];
    memset(lba0, 0xCC, sizeof(lba0));
    rc = storage_blockdev_read(handle, 0, 1, lba0);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    uint8_t zeros[MOCK_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));
    TEST_CHECK_MSG(ctx, memcmp(lba0, zeros, MOCK_BLOCK_SIZE) == 0,
                   "unwritten LBA 0 was modified by write to LBA 1");

    // Out-of-range I/O is rejected.
    TEST_CHECK(ctx, storage_blockdev_read(handle, MOCK_BLOCK_COUNT, 1, in) < 0);
    TEST_CHECK(ctx, storage_blockdev_write(handle, MOCK_BLOCK_COUNT, 1, out) < 0);

    // Inspect the backing store directly: only LBA 1's region must have changed,
    // LBA 0's region must still be all-zeros. This checks isolation at the buffer
    // level, independent of the read path (which the LBA-0 read above relied on).
    // `zeros` is the all-zero reference declared above.
    TEST_CHECK_MSG(ctx, memcmp(mock_backing + 0, zeros, MOCK_BLOCK_SIZE) == 0,
                   "backing LBA 0 region was modified by write to LBA 1");
    TEST_CHECK_MSG(ctx, memcmp(mock_backing + MOCK_BLOCK_SIZE, out, MOCK_BLOCK_SIZE) == 0,
                   "backing LBA 1 region does not hold the written data");

    storage_unregister_blockdev(handle);
}

// storage_blockdev_get enumerates registered devices by index. Register a
// device, then confirm it is reachable by index and that the handle it returns
// is the same one register handed back (and exposes the registered descriptor).
static void test_blockdev_get_by_index(test_ctx_t *ctx) {
    int before = storage_blockdev_count();

    storage_blockdev_t desc;
    make_mock_desc(&desc);

    void *handle = NULL;
    int rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, handle != NULL);

    // The new device is the last in the list (index == prior count).
    void *by_idx = storage_blockdev_get(before);
    TEST_CHECK_EQ_PTR(ctx, by_idx, handle);

    const storage_blockdev_t *info = storage_blockdev_info(by_idx);
    TEST_CHECK(ctx, info != NULL);
    if (info != NULL)
        TEST_CHECK(ctx, strcmp(info->name, "mock0") == 0);

    // Out-of-range indices return NULL rather than reading past the list.
    TEST_CHECK_EQ_PTR(ctx, storage_blockdev_get(-1), NULL);
    TEST_CHECK_EQ_PTR(ctx, storage_blockdev_get(storage_blockdev_count()), NULL);

    storage_unregister_blockdev(handle);
}

// --- Filesystem-provider probe/claim plumbing ---------------------------------
//
// A provider registers a probe callback; CoreStorage offers it every block
// device (those present at provider-registration time, and each one registered
// afterwards). The provider returns 0 to claim a device. These statics let the
// probe record that it ran and on which device, so the test can assert the
// probe/claim path actually fired.

static int probe_call_count;
static void *probe_last_dev;

// fs providers cannot be unregistered (the registry has no removal API), so a
// provider registered by one test stays live for later tests. To keep the
// claim/decline tests isolated, each provider only acts on a device with a
// matching marker name and ignores everything else -- so a stale provider from
// an earlier test can never claim a later test's device out from under it.
#define CLAIM_DEV_NAME "claimme"
#define DECLINE_DEV_NAME "declineme"

static const storage_blockdev_t *probe_info(void *blockdev_handle) {
    return storage_blockdev_info(blockdev_handle);
}

// Claims a device named CLAIM_DEV_NAME (returns 0); ignores all others.
static int mock_probe_claim(void *blockdev_handle) {
    const storage_blockdev_t *info = probe_info(blockdev_handle);
    if (info == NULL || strcmp(info->name, CLAIM_DEV_NAME) != 0)
        return -1;  // not ours -- decline
    probe_call_count++;
    probe_last_dev = blockdev_handle;
    return 0;
}

// Recognises DECLINE_DEV_NAME (so we can assert the probe ran) but always
// declines it (returns <0), leaving the device unclaimed.
static int mock_probe_decline(void *blockdev_handle) {
    const storage_blockdev_t *info = probe_info(blockdev_handle);
    if (info == NULL || strcmp(info->name, DECLINE_DEV_NAME) != 0)
        return -1;
    probe_call_count++;
    probe_last_dev = blockdev_handle;
    return -1;
}

// A block device registered *after* a claiming provider must be offered to that
// provider's probe, and claiming it must set the device's `claimed` flag.
static void test_fsprovider_probe_claim(test_ctx_t *ctx) {
    probe_call_count = 0;
    probe_last_dev = NULL;

    storage_fsprovider_t prov;
    memset(&prov, 0, sizeof(prov));
    strncpy(prov.name, "mockfs", sizeof(prov.name) - 1);
    prov.probe = mock_probe_claim;

    int rc = storage_register_fsprovider(&prov);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // Now register a fresh block device named so this provider claims it; its
    // probe should run on it.
    storage_blockdev_t desc;
    make_mock_desc(&desc);
    memset(desc.name, 0, sizeof(desc.name));
    strncpy(desc.name, CLAIM_DEV_NAME, sizeof(desc.name) - 1);

    void *handle = NULL;
    rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, handle != NULL);

    TEST_CHECK_MSG(ctx, probe_call_count >= 1,
                   "fs provider probe was never invoked for a newly registered device");
    TEST_CHECK_EQ_PTR(ctx, probe_last_dev, handle);

    // Claiming (probe returned 0) must mark the device claimed.
    const storage_blockdev_t *info = storage_blockdev_info(handle);
    TEST_CHECK(ctx, info != NULL);
    if (info != NULL)
        TEST_CHECK_MSG(ctx, info->claimed != 0,
                       "device was not marked claimed after probe returned 0");

    storage_unregister_blockdev(handle);
}

// A declining provider (probe returns <0) must still have its probe invoked, but
// the device must remain unclaimed.
static void test_fsprovider_probe_decline(test_ctx_t *ctx) {
    probe_call_count = 0;
    probe_last_dev = NULL;

    storage_fsprovider_t prov;
    memset(&prov, 0, sizeof(prov));
    strncpy(prov.name, "declinefs", sizeof(prov.name) - 1);
    prov.probe = mock_probe_decline;

    int rc = storage_register_fsprovider(&prov);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    storage_blockdev_t desc;
    make_mock_desc(&desc);
    memset(desc.name, 0, sizeof(desc.name));
    strncpy(desc.name, DECLINE_DEV_NAME, sizeof(desc.name) - 1);

    void *handle = NULL;
    rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, handle != NULL);

    TEST_CHECK_MSG(ctx, probe_call_count >= 1,
                   "declining fs provider probe was never invoked");
    TEST_CHECK_EQ_PTR(ctx, probe_last_dev, handle);

    const storage_blockdev_t *info = storage_blockdev_info(handle);
    TEST_CHECK(ctx, info != NULL);
    if (info != NULL)
        TEST_CHECK_MSG(ctx, info->claimed == 0,
                       "device was marked claimed even though probe declined it");

    storage_unregister_blockdev(handle);
}

void corestorage_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "register_unregister",
            .fn = test_register_unregister,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "info_roundtrip",
            .fn = test_info_roundtrip,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "read_write_roundtrip",
            .fn = test_read_write_roundtrip,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "blockdev_get_by_index",
            .fn = test_blockdev_get_by_index,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "fsprovider_probe_claim",
            .fn = test_fsprovider_probe_claim,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
    {
        test_def_t t = {
            .suite = "CoreStorage",
            .name = "fsprovider_probe_decline",
            .fn = test_fsprovider_probe_decline,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
