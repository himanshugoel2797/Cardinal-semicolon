// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host differential / unit test harness for the Wasm interpreter.
//
// Fixtures are hand-authored .wat compiled to .wasm by wat2wasm at build time
// (see build-and-run.sh) and embedded; the test workstream fills in coverage.
// This file provides the minimal framework + a smoke test so the foundation
// builds and runs green while the decode/exec layers are stubs.

#include "wasm.h"
#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                      \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                   \
    } while (0)

// Smoke: malformed bytes must be rejected, not crash. Works against the stub.
static void test_smoke_reject_garbage(void) {
    const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(garbage, sizeof(garbage), &err);
    CHECK(m == NULL, "garbage decoded to non-NULL module");
    CHECK(err != WASM_OK, "garbage decode reported WASM_OK");
    if (m) wasm_module_free(m);
}

int main(void) {
    printf("[wasm-test] running\n");
    test_smoke_reject_garbage();

    // Decode/exec tests are added by the test workstream as the layers land.
    // They follow the shape: decode a fixture -> instantiate -> wasm_call ->
    // wasm_resume to WASM_RUN_DONE -> assert wasm_results(), and drive
    // WASM_RUN_SUSPENDED through wasm_provide() for import tests.

    printf("[wasm-test] %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
