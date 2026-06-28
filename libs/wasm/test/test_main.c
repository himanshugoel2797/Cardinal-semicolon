// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host integration test harness for the Wasm interpreter: drives the full
// decode -> instantiate -> call -> resume pipeline over real modules compiled
// from the .wat fixtures by wat2wasm (build-and-run.sh embeds each as a
// <name>_wasm[] byte array). The decode and exec layers are unit-tested
// separately; this proves they compose. Built with ASan+UBSan.

#include "wasm.h"
#include <stdio.h>
#include <string.h>

#include "add.wasm.h"
#include "sumloop.wasm.h"
#include "memtest.wasm.h"
#include "callind.wasm.h"
#include "hostimp.wasm.h"
#include "f64ops.wasm.h"

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

// Decode+instantiate a fixture (no imports). Returns NULL on failure (asserts).
static wasm_instance_t *load(const unsigned char *bytes, size_t len,
                             const wasm_import_def_t *imp, uint32_t n_imp) {
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(bytes, len, &err);
    CHECK(m != NULL && err == WASM_OK, "decode failed err=%d", err);
    if (!m) return NULL;
    wasm_instance_t *inst = wasm_instantiate(m, imp, n_imp, &err);
    CHECK(inst != NULL && err == WASM_OK, "instantiate failed err=%d", err);
    return inst;   // module leaks intentionally here only if inst==NULL; tests are short-lived
}

// Run a started call to completion (no imports expected), return the single i32.
static int32_t run_i32(wasm_instance_t *inst) {
    for (;;) {
        wasm_run_status_t s = wasm_resume(inst, 1000000);
        if (s == WASM_RUN_FUEL) continue;
        CHECK(s == WASM_RUN_DONE, "run did not complete: status=%d trap=%d",
              s, wasm_trap(inst));
        break;
    }
    wasm_value_t out[4];
    uint32_t n = wasm_results(inst, out, 4);
    CHECK(n == 1, "expected 1 result, got %u", n);
    return out[0].i32;
}

static void test_add(void) {
    wasm_instance_t *inst = load(add_wasm, sizeof(add_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t args[2] = {{.i32 = 40}, {.i32 = 2}};
    CHECK(wasm_call(inst, "add", args, 2) == WASM_OK, "wasm_call add");
    CHECK(run_i32(inst) == 42, "add(40,2) != 42");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

static void test_sumloop(void) {
    wasm_instance_t *inst = load(sumloop_wasm, sizeof(sumloop_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t a = {.i32 = 100};
    CHECK(wasm_call(inst, "sum", &a, 1) == WASM_OK, "wasm_call sum");
    CHECK(run_i32(inst) == 5050, "sum(1..100) != 5050");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

// Fuel slicing: the same loop must yield WASM_RUN_FUEL several times and still
// reach the same answer -- the property that lets a Lisp host time-slice a guest.
static void test_fuel_slicing(void) {
    wasm_instance_t *inst = load(sumloop_wasm, sizeof(sumloop_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t a = {.i32 = 1000};
    wasm_call(inst, "sum", &a, 1);
    int slices = 0;
    wasm_run_status_t s;
    while ((s = wasm_resume(inst, 5)) == WASM_RUN_FUEL) slices++;
    CHECK(s == WASM_RUN_DONE, "sliced run did not finish: %d", s);
    CHECK(slices > 10, "expected many fuel slices, got %d", slices);
    wasm_value_t out[1];
    wasm_results(inst, out, 1);
    CHECK(out[0].i32 == 500500, "sliced sum(1..1000) != 500500");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

static void test_memory(void) {
    wasm_instance_t *inst = load(memtest_wasm, sizeof(memtest_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t args[2] = {{.i32 = 64}, {.i32 = 0xCAFEBABE}};
    wasm_call(inst, "rw", args, 2);
    CHECK((uint32_t)run_i32(inst) == 0xCAFEBABE, "mem store/load roundtrip");
    // data segment landed (little-endian 0x44332211 at offset 16)
    wasm_call(inst, "rdata", NULL, 0);
    CHECK((uint32_t)run_i32(inst) == 0x44332211, "data segment contents");
    // zero-copy host view sees the written word at offset 64
    size_t mlen = 0;
    uint8_t *mem = wasm_memory(inst, &mlen);
    CHECK(mem && mlen >= 68, "memory view");
    uint32_t w;
    memcpy(&w, mem + 64, 4);
    CHECK(w == 0xCAFEBABE, "host sees guest-written memory");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

static void test_call_indirect(void) {
    wasm_instance_t *inst = load(callind_wasm, sizeof(callind_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t add[3] = {{.i32 = 10}, {.i32 = 3}, {.i32 = 0}}; // index 0 = add
    wasm_call(inst, "apply", add, 3);
    CHECK(run_i32(inst) == 13, "call_indirect add");
    wasm_value_t sub[3] = {{.i32 = 10}, {.i32 = 3}, {.i32 = 1}}; // index 1 = sub
    wasm_call(inst, "apply", sub, 3);
    CHECK(run_i32(inst) == 7, "call_indirect sub");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

static wasm_trap_t host_dbl(wasm_instance_t *inst, void *user,
                            const wasm_value_t *args, wasm_value_t *results) {
    (void)inst; (void)user;
    results[0].i32 = args[0].i32 * 2;
    return WASM_TRAP_NONE;
}

// Synchronous import: serviced inline in C, no suspend.
static void test_import_sync(void) {
    wasm_import_def_t imp = {"env", "dbl", host_dbl, NULL, 1};
    wasm_instance_t *inst = load(hostimp_wasm, sizeof(hostimp_wasm), &imp, 1);
    if (!inst) return;
    wasm_value_t a = {.i32 = 21};
    wasm_call(inst, "calldbl", &a, 1);
    CHECK(run_i32(inst) == 42, "sync import dbl(21)");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

// Host-serviced import: the call suspends to the driver, which provides results.
static void test_import_suspend(void) {
    wasm_import_def_t imp = {"env", "dbl", NULL /*host-serviced*/, NULL, 7};
    wasm_instance_t *inst = load(hostimp_wasm, sizeof(hostimp_wasm), &imp, 1);
    if (!inst) return;
    wasm_value_t a = {.i32 = 21};
    wasm_call(inst, "calldbl", &a, 1);
    wasm_run_status_t s = wasm_resume(inst, 1000000);
    CHECK(s == WASM_RUN_SUSPENDED, "expected suspend, got %d", s);
    const wasm_pending_t *p = wasm_pending(inst);
    CHECK(p->host_id == 7, "pending host_id");
    CHECK(p->n_args == 1 && p->args[0].i32 == 21, "pending args");
    CHECK(p->n_results == 1, "pending n_results");
    wasm_value_t r = {.i32 = p->args[0].i32 * 2};
    wasm_provide(inst, &r, 1);
    CHECK(run_i32(inst) == 42, "suspend/provide import dbl(21)");
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

static void test_f64(void) {
    wasm_instance_t *inst = load(f64ops_wasm, sizeof(f64ops_wasm), NULL, 0);
    if (!inst) return;
    wasm_value_t args[2] = {{.f64 = 3.0}, {.f64 = 4.0}};
    wasm_call(inst, "hyp", args, 2);
    for (;;) { wasm_run_status_t s = wasm_resume(inst, 1000000);
        if (s == WASM_RUN_FUEL) continue;
        CHECK(s == WASM_RUN_DONE, "f64 run status %d", s); break; }
    wasm_value_t out[1];
    wasm_results(inst, out, 1);
    CHECK(out[0].f64 == 5.0, "hyp(3,4) != 5 (got %f)", out[0].f64);
    wasm_module_t *m_ = wasm_instance_module(inst);
    wasm_instance_free(inst);
    wasm_module_free(m_);
}

// Smoke: malformed bytes are rejected, not crashed.
static void test_reject_garbage(void) {
    const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(garbage, sizeof(garbage), &err);
    CHECK(m == NULL && err != WASM_OK, "garbage was not rejected");
    if (m) wasm_module_free(m);
}

int main(void) {
    printf("[wasm-test] running\n");
    test_reject_garbage();
    test_add();
    test_sumloop();
    test_fuel_slicing();
    test_memory();
    test_call_indirect();
    test_import_sync();
    test_import_suspend();
    test_f64();
    printf("[wasm-test] %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
