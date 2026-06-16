/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdbool.h>

void coreaudio_register_tests(void);

// Guard registration so module_init is idempotent: re-invoking it (e.g. from
// the CoreAudio smoke test, which asserts the entry point returns success)
// must not register the test suite a second time.
static bool g_tests_registered = false;

int module_init() {

    if (!g_tests_registered) {
        g_tests_registered = true;
        coreaudio_register_tests();
    }
    return 0;
}
