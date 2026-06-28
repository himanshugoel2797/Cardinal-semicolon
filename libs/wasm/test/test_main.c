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

// ---- Validation ---------------------------------------------------------
//
// The valid fixtures must pass wasm_module_validate; a set of modules that are
// structurally decodable but ill-typed must be rejected with WASM_ERR_VALIDATE.
// These mirror the differential corpus checked against wabt's wasm-validate.

// Decodable-but-invalid modules (compiled from .wat with wat2wasm --no-check):
//   i_addf64    : i32.add on two f64 operands
//   i_setimm    : global.set on an immutable global
//   i_loadnomem : i32.load with no memory declared
//   i_brdepth   : br to an out-of-range label depth
static const unsigned char inv_addf64[] = {
    0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,2,1,0,7,5,1,1,102,0,0,
    10,23,1,21,0,68,0,0,0,0,0,0,240,63,68,0,0,0,0,0,0,0,64,106,11};
static const unsigned char inv_setimm[] = {
    0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,6,6,1,127,0,65,0,11,
    7,5,1,1,102,0,0,10,8,1,6,0,65,1,36,0,11};
static const unsigned char inv_loadnomem[] = {
    0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,3,2,1,0,7,5,1,1,102,0,0,
    10,9,1,7,0,32,0,40,2,0,11};
static const unsigned char inv_brdepth[] = {
    0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,7,5,1,1,102,0,0,
    10,9,1,7,0,2,64,12,5,11,11};

// Validate a decoded module from raw bytes; *out_decoded set true if decode
// succeeded. Returns the validate result (only meaningful when decoded).
static wasm_result_t validate_bytes(const unsigned char *b, size_t n,
                                    int *out_decoded) {
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(b, n, &err);
    *out_decoded = (m != NULL && err == WASM_OK);
    if (!m) return err;
    wasm_result_t v = wasm_module_validate(m);
    wasm_module_free(m);
    return v;
}

static void check_valid_fixture(const unsigned char *b, size_t n, const char *nm) {
    int decoded = 0;
    wasm_result_t v = validate_bytes(b, n, &decoded);
    CHECK(decoded, "valid fixture %s failed to decode", nm);
    if (decoded) CHECK(v == WASM_OK, "valid fixture %s rejected by validate (%d)", nm, v);
}

static void check_invalid(const unsigned char *b, size_t n, const char *nm) {
    int decoded = 0;
    wasm_result_t v = validate_bytes(b, n, &decoded);
    // It must decode (these are decodable) and then fail validation.
    CHECK(decoded, "invalid fixture %s unexpectedly failed to decode", nm);
    if (decoded)
        CHECK(v == WASM_ERR_VALIDATE, "invalid fixture %s was accepted (%d)", nm, v);
}

static void test_validation(void) {
    // (a) the valid fixtures all pass validation
    check_valid_fixture(add_wasm, sizeof(add_wasm), "add");
    check_valid_fixture(sumloop_wasm, sizeof(sumloop_wasm), "sumloop");
    check_valid_fixture(memtest_wasm, sizeof(memtest_wasm), "memtest");
    check_valid_fixture(callind_wasm, sizeof(callind_wasm), "callind");
    check_valid_fixture(hostimp_wasm, sizeof(hostimp_wasm), "hostimp");
    check_valid_fixture(f64ops_wasm, sizeof(f64ops_wasm), "f64ops");
    // (b) ill-typed-but-decodable modules are rejected
    check_invalid(inv_addf64, sizeof(inv_addf64), "i32.add on f64");
    check_invalid(inv_setimm, sizeof(inv_setimm), "global.set immutable");
    check_invalid(inv_loadnomem, sizeof(inv_loadnomem), "load without memory");
    check_invalid(inv_brdepth, sizeof(inv_brdepth), "br out-of-range depth");
}

// Smoke: malformed bytes are rejected, not crashed.
static void test_reject_garbage(void) {
    const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef};
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(garbage, sizeof(garbage), &err);
    CHECK(m == NULL && err != WASM_OK, "garbage was not rejected");
    if (m) wasm_module_free(m);
}

// ---- Interpreter memory-safety regression tests -------------------------
//
// These hand-encoded modules DECODE cleanly and instantiate, but their bodies
// name an out-of-range index (local / global / call funcidx). They are NOT run
// through wasm_module_validate -- the whole point is that the *interpreter* must
// be memory-safe on its own (validation is a separate, optional pass). Each must
// produce WASM_RUN_TRAPPED rather than reading/writing out of bounds. Without
// the runtime bounds checks these crash under ASan/UBSan (verified).
//
// Module skeleton shared by all three (only the body opcode pair differs):
//   type[0] = () -> i32 ; func[0] : type 0 ; export "f" = func 0
//   code[0] body = (no locals) <OPCODE> <IMM=5> end
// With 0 locals, 0 globals, and a single function, index 5 is always OOB.
#define WASM_OOB_MODULE(op)                                                     \
    { 0,97,115,109,1,0,0,0,    /* magic + version */                           \
      1,5, 1,0x60,0,1,0x7F,    /* type: ()->i32 */                            \
      3,2, 1,0,                /* func: 1 func, type 0 */                      \
      7,5, 1,1,'f',0,0,        /* export "f" = func 0 */                       \
      10,6, 1,4, 0, (op),5, 0x0B } /* code: <op> 5 ; end */

// Instantiate (NO validate) and run; assert the run traps cleanly.
static void check_traps_at_runtime(const unsigned char *b, size_t n,
                                   const char *nm) {
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(b, n, &err);
    CHECK(m != NULL && err == WASM_OK, "%s: expected clean decode (err=%d)", nm, err);
    if (!m) return;
    // Deliberately skip wasm_module_validate -- prove the interpreter is safe.
    wasm_instance_t *inst = wasm_instantiate(m, NULL, 0, &err);
    CHECK(inst != NULL && err == WASM_OK, "%s: instantiate failed (err=%d)", nm, err);
    if (inst) {
        CHECK(wasm_call(inst, "f", NULL, 0) == WASM_OK, "%s: wasm_call", nm);
        wasm_run_status_t s;
        int iters = 0;
        do { s = wasm_resume(inst, 1000000); } while (s == WASM_RUN_FUEL && ++iters < 100);
        CHECK(s == WASM_RUN_TRAPPED, "%s: expected TRAPPED, got status=%d", nm, s);
        wasm_instance_free(inst);
    }
    wasm_module_free(m);
}

static void test_oob_local_index(void) {
    static const unsigned char m[] = WASM_OOB_MODULE(0x20 /*local.get*/);
    check_traps_at_runtime(m, sizeof(m), "local.get OOB index");
}
static void test_oob_global_index(void) {
    static const unsigned char m[] = WASM_OOB_MODULE(0x23 /*global.get*/);
    check_traps_at_runtime(m, sizeof(m), "global.get OOB index");
}
static void test_oob_call_funcidx(void) {
    static const unsigned char m[] = WASM_OOB_MODULE(0x10 /*call*/);
    check_traps_at_runtime(m, sizeof(m), "call OOB funcidx");
}

// local.set with an OOB index (a *write* path) must also trap, not corrupt the
// stack/heap. Body: i32.const 7 ; local.set 5 ; end (0 locals).
static void test_oob_local_set_index(void) {
    static const unsigned char m[] = {
        0,97,115,109,1,0,0,0,
        1,4, 1,0x60,0,0,          /* type: ()->() */
        3,2, 1,0,
        7,5, 1,1,'f',0,0,
        10,8, 1,6, 0, 0x41,7, 0x21,5, 0x0B }; /* i32.const 7; local.set 5; end */
    check_traps_at_runtime(m, sizeof(m), "local.set OOB index");
}

// local.tee with a VALID index but an empty value stack (vsp==0) must trap on
// the peek (item 2), not read vstack[-1]. We drive vsp to 0 by dropping past the
// locals region (only possible without the validator). Body (1 i32 local):
//   local.get 0 ; drop ; drop ; local.tee 0 ; end
// The second drop pops the local slot itself (vsp 1->0); local.tee 0 then peeks
// an empty stack while its index (0) is in range.
static void test_tee_underflow(void) {
    static const unsigned char m[] = {
        0,97,115,109,1,0,0,0,
        1,4, 1,0x60,0,0,                              /* type: ()->() */
        3,2, 1,0,
        7,5, 1,1,'f',0,0,
        10,12, 1,10, 1,1,0x7F,                        /* 1 i32 local */
        0x20,0, 0x1A, 0x1A, 0x22,0, 0x0B };           /* get0;drop;drop;tee0;end */
    check_traps_at_runtime(m, sizeof(m), "local.tee stack underflow");
}

// ---- Decode-time rejection regression tests (HIGH items) ----------------
// These must be rejected at DECODE time (item 6/7/8) -- the index is out of
// range in a section, so the module never reaches the interpreter.
static void check_decode_rejected(const unsigned char *b, size_t n, const char *nm) {
    wasm_result_t err = WASM_OK;
    wasm_module_t *m = wasm_module_decode(b, n, &err);
    CHECK(m == NULL && err == WASM_ERR_DECODE, "%s: expected decode reject (err=%d)", nm, err);
    if (m) wasm_module_free(m);
}

static void test_decode_rejects_bad_export(void) {
    // export "f" names func index 5, but only func 0 exists (item 6).
    static const unsigned char m[] = {
        0,97,115,109,1,0,0,0,
        1,5, 1,0x60,0,1,0x7F,
        3,2, 1,0,
        7,5, 1,1,'f',0,5,        /* export func index 5 (OOB) */
        10,6, 1,4, 0, 0x41,0, 0x0B };
    check_decode_rejected(m, sizeof(m), "export OOB funcidx");
}

static void test_decode_rejects_bad_elem(void) {
    // elem segment seeds the table with func index 9, but only func 0 exists
    // (item 7). Needs a table to be present.
    static const unsigned char m[] = {
        0,97,115,109,1,0,0,0,
        1,5, 1,0x60,0,1,0x7F,
        3,2, 1,0,
        4,4, 1,0x70,0,1,         /* table: 1 funcref table, min 1 */
        9,7, 1, 0, 0x41,0,0x0B, 1,9, /* elem: active, offset 0, [funcidx 9] */
        10,6, 1,4, 0, 0x41,0, 0x0B };
    check_decode_rejected(m, sizeof(m), "elem OOB funcidx");
}

static void test_decode_rejects_bad_import_type(void) {
    // import "env"."f" as a func of type index 5, but only type 0 exists (item 8).
    static const unsigned char m[] = {
        0,97,115,109,1,0,0,0,
        1,4, 1,0x60,0,0,                          /* 1 type ()->() */
        2,9, 1, 3,'e','n','v', 1,'f', 0, 5 };     /* import env.f func type 5 */
    check_decode_rejected(m, sizeof(m), "import OOB type index");
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
    test_validation();
    // Interpreter memory-safety regressions (run WITHOUT validation).
    test_oob_local_index();
    test_oob_local_set_index();
    test_oob_global_index();
    test_oob_call_funcidx();
    test_tee_underflow();
    // Decode-time rejections (HIGH items 6/7/8).
    test_decode_rejects_bad_export();
    test_decode_rejects_bad_elem();
    test_decode_rejects_bad_import_type();
    printf("[wasm-test] %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
