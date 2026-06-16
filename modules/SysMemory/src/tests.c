// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "SysMemory/memory.h"
#include "SysTest/test.h"

// stack_alloc is currently a stub that always returns NULL regardless of size
// or the kernel flag. These checks document that contract so a future real
// implementation has to consciously update the tests.
static void test_stack_alloc_stub(test_ctx_t *ctx)
{
    TEST_CHECK_MSG(ctx, stack_alloc(4096, true) == NULL,
                   "stack_alloc(4096, true) stub returns NULL");
    TEST_CHECK_MSG(ctx, stack_alloc(4096, false) == NULL,
                   "stack_alloc(4096, false) stub returns NULL");
}

// Heap stress: allocate a batch of small blocks, scribble over each so any
// overlap or short allocation would corrupt a neighbour, verify the contents
// survived, then free everything.
static void test_heap_malloc_free(test_ctx_t *ctx)
{
    enum { N = 8, SZ = 64 };
    uint8_t *blocks[N];

    for (int i = 0; i < N; i++) {
        blocks[i] = (uint8_t *)malloc(SZ);
        TEST_CHECK_MSG(ctx, blocks[i] != NULL, "malloc(64) returns non-NULL");
        if (blocks[i] != NULL)
            memset(blocks[i], (uint8_t)(0xA0 + i), SZ);
    }

    for (int i = 0; i < N; i++) {
        if (blocks[i] == NULL)
            continue;
        bool ok = true;
        for (int j = 0; j < SZ; j++)
            if (blocks[i][j] != (uint8_t)(0xA0 + i))
                ok = false;
        TEST_CHECK_MSG(ctx, ok, "block contents survived intact");
    }

    for (int i = 0; i < N; i++)
        free(blocks[i]);

    // free(NULL) is a documented no-op (production guards it); exercise it so
    // the edge case is actually covered.
    free(NULL);
    TEST_CHECK_MSG(ctx, true, "free(NULL) returned without faulting");
}

// realloc: grow 32 -> 64 (must preserve the original bytes), then shrink to 16
// (kept in place, original prefix preserved), then free.
static void test_realloc_grow_shrink(test_ctx_t *ctx)
{
    uint8_t *p = (uint8_t *)malloc(32);
    TEST_CHECK_MSG(ctx, p != NULL, "initial malloc(32) returns non-NULL");
    if (p == NULL)
        return;

    for (int i = 0; i < 32; i++)
        p[i] = (uint8_t)(i + 1);

    uint8_t *grown = (uint8_t *)realloc(p, 64);
    TEST_CHECK_MSG(ctx, grown != NULL, "realloc grow 32->64 returns non-NULL");
    if (grown == NULL) {
        free(p);
        return;
    }

    bool grow_ok = true;
    for (int i = 0; i < 32; i++)
        if (grown[i] != (uint8_t)(i + 1))
            grow_ok = false;
    TEST_CHECK_MSG(ctx, grow_ok, "realloc grow preserved original bytes");

    uint8_t *shrunk = (uint8_t *)realloc(grown, 16);
    TEST_CHECK_MSG(ctx, shrunk != NULL, "realloc shrink 64->16 returns non-NULL");
    if (shrunk == NULL) {
        free(grown);
        return;
    }

    // The allocator's realloc keeps a shrink in place (old_len >= req short
    // circuit returns the same pointer; it never splits nodes on shrink), so
    // the pointer must be identical -- not merely a copy with the same prefix.
    TEST_CHECK_MSG(ctx, shrunk == grown,
                   "realloc shrink kept the block in place (same pointer)");

    bool shrink_ok = true;
    for (int i = 0; i < 16; i++)
        if (shrunk[i] != (uint8_t)(i + 1))
            shrink_ok = false;
    TEST_CHECK_MSG(ctx, shrink_ok, "realloc shrink preserved leading bytes");

    free(shrunk);
}

void sysmemory_register_tests(void)
{
    if (!test_mode_active())
        return;

    test_def_t stack_test = {
        .suite = "SysMemory",
        .name = "stack_alloc_stub",
        .fn = test_stack_alloc_stub,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&stack_test);

    test_def_t heap_test = {
        .suite = "SysMemory",
        .name = "heap_malloc_free",
        .fn = test_heap_malloc_free,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&heap_test);

    test_def_t realloc_test = {
        .suite = "SysMemory",
        .name = "realloc_grow_shrink",
        .fn = test_realloc_grow_shrink,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&realloc_test);
}
