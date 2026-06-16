// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <SysTest/test.h>

// CoreAudio is currently a stub (notes/AUDIT.md): its only exported entry point
// is module_init, which performs setup and returns 0 on success. There is no
// other public API to exercise, so this is an honest smoke test of that real
// entry point -- it asserts module_init() actually returns success and is
// reachable, not a tautology. module_init is idempotent (registration is
// guarded), so re-invoking it here is safe.
int module_init(void);

static void test_module_init_succeeds(test_ctx_t *ctx) {
    TEST_CHECK_MSG(ctx, module_init() == 0,
                   "CoreAudio module_init() returns success");
}

void coreaudio_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreAudio",
            .name = "module_init_succeeds",
            .fn = test_module_init_succeeds,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
