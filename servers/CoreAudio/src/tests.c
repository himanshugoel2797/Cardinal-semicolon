// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <SysTest/test.h>

// CoreAudio is a stub pending implementation; verifies module loaded.
static void test_module_loaded(test_ctx_t *ctx) {
    TEST_CHECK(ctx, 1 == 1);
}

void coreaudio_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreAudio",
            .name = "module_loaded",
            .fn = test_module_loaded,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
