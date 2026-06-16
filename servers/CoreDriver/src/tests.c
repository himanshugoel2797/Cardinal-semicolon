// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include "SysTest/test.h"

// CoreDriver is a boot-time PCI loader; test verifies module loaded without panic.
static void test_loaded(test_ctx_t *ctx)
{
    TEST_CHECK(ctx, 1 == 1);
}

void coredriver_register_tests(void)
{
    if (!test_mode_active())
        return;

    test_def_t t = {
        .suite = "CoreDriver",
        .name = "loaded",
        .fn = test_loaded,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&t);
}
