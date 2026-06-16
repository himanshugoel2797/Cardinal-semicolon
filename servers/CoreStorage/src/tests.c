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
// for byte.
static void test_read_write_roundtrip(test_ctx_t *ctx) {
    storage_blockdev_t desc;
    make_mock_desc(&desc);

    void *handle = NULL;
    int rc = storage_register_blockdev(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    if (handle == NULL) {
        TEST_FAIL(ctx, "register returned NULL handle");
        return;
    }

    uint8_t out[MOCK_BLOCK_SIZE];
    for (uint32_t i = 0; i < MOCK_BLOCK_SIZE; i++)
        out[i] = (uint8_t)(i ^ 0xA5);

    rc = storage_blockdev_write(handle, 1, 1, out);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    uint8_t in[MOCK_BLOCK_SIZE];
    memset(in, 0, sizeof(in));
    rc = storage_blockdev_read(handle, 1, 1, in);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK(ctx, memcmp(out, in, MOCK_BLOCK_SIZE) == 0);

    // Out-of-range I/O is rejected.
    TEST_CHECK(ctx, storage_blockdev_read(handle, MOCK_BLOCK_COUNT, 1, in) < 0);
    TEST_CHECK(ctx, storage_blockdev_write(handle, MOCK_BLOCK_COUNT, 1, out) < 0);

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
}
