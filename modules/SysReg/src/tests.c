// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <cardinal/cs_error.h>

#include "registry.h"
#include "SysTest/test.h"

// SysTest test suite for SysReg. All tests are pure logic against the in-memory
// registry and never block, so they run TEST_RUN_INLINE. They operate strictly
// under /TestSysReg/ so they never touch production HW/* keys.

#define TEST_ROOT "TestSysReg"

// Create the isolated test directory. Treat CS_EXISTS as fine so the suite is
// re-runnable within a single boot.
static void test_createdirectory(test_ctx_t *ctx)
{
    cs_error err = registry_createdirectory("", TEST_ROOT);
    TEST_CHECK_MSG(ctx, err == CS_OK || err == CS_EXISTS,
                   "createdirectory /TestSysReg");
}

// uint add + read roundtrip.
static void test_key_uint(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT, "u", 0xDEADBEEFCAFEull),
                    CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(TEST_ROOT, "u", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, 0xDEADBEEFCAFEull);

    registry_removekey(TEST_ROOT, "u");
}

// int (signed) add + read roundtrip, including a negative value.
static void test_key_int(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_int(TEST_ROOT, "i", -42), CS_OK);

    int64_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_int(TEST_ROOT, "i", &v), CS_OK);
    TEST_CHECK_MSG(ctx, v == -42, "signed value roundtrip");

    registry_removekey(TEST_ROOT, "i");
}

// bool add + read roundtrip.
static void test_key_bool(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_bool(TEST_ROOT, "b", true), CS_OK);

    bool v = false;
    TEST_CHECK_EQ_U(ctx, registry_readkey_bool(TEST_ROOT, "b", &v), CS_OK);
    TEST_CHECK(ctx, v == true);

    registry_removekey(TEST_ROOT, "b");
}

// str add + read roundtrip.
static void test_key_str(test_ctx_t *ctx)
{
    const char *expect = "hello-registry";
    TEST_CHECK_EQ_U(ctx, registry_addkey_str(TEST_ROOT, "s", expect), CS_OK);

    char buf[64];
    size_t len = sizeof(buf);
    TEST_CHECK_EQ_U(ctx, registry_readkey_str(TEST_ROOT, "s", buf, &len), CS_OK);
    TEST_CHECK_EQ_U(ctx, len, strlen(expect));
    TEST_CHECK_MSG(ctx, strncmp(buf, expect, strlen(expect)) == 0,
                   "string value roundtrip");

    registry_removekey(TEST_ROOT, "s");
}

// ptr add + read roundtrip.
static void test_key_ptr(test_ctx_t *ctx)
{
    uintptr_t expect = (uintptr_t)0x1000;
    TEST_CHECK_EQ_U(ctx, registry_addkey_ptr(TEST_ROOT, "p", expect), CS_OK);

    uintptr_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_ptr(TEST_ROOT, "p", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, expect);

    registry_removekey(TEST_ROOT, "p");
}

// Reading a key with the wrong typed accessor must report a type mismatch.
static void test_type_mismatch(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT, "tm", 7), CS_OK);

    int64_t iv = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_int(TEST_ROOT, "tm", &iv),
                    CS_TYPEMISMATCH);

    registry_removekey(TEST_ROOT, "tm");
}

// Update a uint key in place. The public API has no writekey verb and re-adding
// an existing key fails, so an update is remove-then-add; verify the new value
// is observed afterwards.
static void test_writekey_uint_update(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT, "w", 1), CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(TEST_ROOT, "w", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, 1);

    TEST_CHECK_EQ_U(ctx, registry_removekey(TEST_ROOT, "w"), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT, "w", 2), CS_OK);

    v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(TEST_ROOT, "w", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, 2);

    registry_removekey(TEST_ROOT, "w");
}

// removekey deletes the key: a subsequent read reports it does not exist, and a
// second removekey reports the same.
static void test_removekey(test_ctx_t *ctx)
{
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT, "rm", 99), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_removekey(TEST_ROOT, "rm"), CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(TEST_ROOT, "rm", &v), CS_DNE);
    TEST_CHECK_EQ_U(ctx, registry_removekey(TEST_ROOT, "rm"), CS_DNE);
}

// getdirectory + registry_next iteration must visit the keys we just added.
static void test_iteration(test_ctx_t *ctx)
{
    // Use a dedicated subdirectory so the iteration set is deterministic.
    cs_error derr = registry_createdirectory(TEST_ROOT, "iter");
    TEST_CHECK_MSG(ctx, derr == CS_OK || derr == CS_EXISTS,
                   "createdirectory /TestSysReg/iter");

    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT "/iter", "a", 1), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT "/iter", "bb", 2), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_addkey_uint(TEST_ROOT "/iter", "ccc", 3),
                    CS_OK);

    dir_t dir = NULL;
    TEST_CHECK_EQ_U(ctx, registry_getdirectory(TEST_ROOT "/iter", &dir), CS_OK);

    bool saw_a = false, saw_bb = false, saw_ccc = false;
    int count = 0;
    while (registry_next(&dir) == CS_OK)
    {
        char name[MAX_REGISTRY_KEYLEN];
        name[0] = '\0';
        if (registry_readlocal_key(dir, name) != CS_OK)
            continue;
        if (strcmp(name, "a") == 0)
            saw_a = true;
        else if (strcmp(name, "bb") == 0)
            saw_bb = true;
        else if (strcmp(name, "ccc") == 0)
            saw_ccc = true;
        count++;
    }

    TEST_CHECK_MSG(ctx, saw_a, "iteration saw key a");
    TEST_CHECK_MSG(ctx, saw_bb, "iteration saw key bb");
    TEST_CHECK_MSG(ctx, saw_ccc, "iteration saw key ccc");
    TEST_CHECK_MSG(ctx, count >= 3, "iteration visited at least 3 keys");

    registry_removekey(TEST_ROOT "/iter", "a");
    registry_removekey(TEST_ROOT "/iter", "bb");
    registry_removekey(TEST_ROOT "/iter", "ccc");
}

void sysreg_register_tests(void)
{
    if (!test_mode_active())
        return;

    static const test_fn_t fns[] = {
        test_createdirectory,
        test_key_uint,
        test_key_int,
        test_key_bool,
        test_key_str,
        test_key_ptr,
        test_type_mismatch,
        test_writekey_uint_update,
        test_removekey,
        test_iteration,
    };
    static const char *const names[] = {
        "createdirectory",
        "key_uint",
        "key_int",
        "key_bool",
        "key_str",
        "key_ptr",
        "type_mismatch",
        "writekey_uint_update",
        "removekey",
        "iteration",
    };

    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
    {
        test_def_t t = {
            .suite = "SysReg",
            .name = names[i],
            .fn = fns[i],
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
