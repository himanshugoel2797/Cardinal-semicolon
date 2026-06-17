// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// SysTest test suite for the SysObj object model. Every test runs INLINE in the
// runner thread (the object store never blocks). They build and tear down a
// scratch directory under the store root ("/TestSysObj") so they don't depend on
// or perturb anything else in the tree.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "obj.h"
#include "SysTest/test.h"

#define TEST_ROOT ""          // the object store root path
#define TEST_DIR  "TestSysObj"
#define TEST_PATH "TestSysObj"

// Create a fresh scratch directory, removing any stale copy first. Returns true
// on success so the caller can bail out early without cascading failures.
static bool make_scratch(test_ctx_t *ctx) {
    obj_removedirectory(TEST_ROOT, TEST_DIR); // ignore result; may not exist
    cs_error err = obj_createdirectory(TEST_ROOT, TEST_DIR);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    return err == CS_OK;
}

static void drop_scratch(test_ctx_t *ctx) {
    cs_error err = obj_removedirectory(TEST_ROOT, TEST_DIR);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
}

// obj_createdirectory: creating succeeds; creating again reports CS_EXISTS.
static void test_createdirectory(test_ctx_t *ctx) {
    obj_removedirectory(TEST_ROOT, TEST_DIR);

    TEST_CHECK_EQ_U(ctx, obj_createdirectory(TEST_ROOT, TEST_DIR), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_createdirectory(TEST_ROOT, TEST_DIR), CS_EXISTS);

    drop_scratch(ctx);
}

// uint key add + read round-trip.
static void test_key_uint(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "u", 0xDEADBEEFCAFEull), CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, obj_readkey_uint(TEST_PATH, "u", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, 0xDEADBEEFCAFEull);

    drop_scratch(ctx);
}

// int (signed) key add + read round-trip, including a negative value.
static void test_key_int(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_int(TEST_PATH, "i", -1234), CS_OK);

    int64_t v = 0;
    TEST_CHECK_EQ_U(ctx, obj_readkey_int(TEST_PATH, "i", &v), CS_OK);
    TEST_CHECK(ctx, v == -1234);

    drop_scratch(ctx);
}

// bool key add + read round-trip for both values.
static void test_key_bool(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_bool(TEST_PATH, "t", true), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_addkey_bool(TEST_PATH, "f", false), CS_OK);

    bool t = false, f = true;
    TEST_CHECK_EQ_U(ctx, obj_readkey_bool(TEST_PATH, "t", &t), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_readkey_bool(TEST_PATH, "f", &f), CS_OK);
    TEST_CHECK(ctx, t == true);
    TEST_CHECK(ctx, f == false);

    drop_scratch(ctx);
}

// str key add + read round-trip; verify the read-back contents and length.
static void test_key_str(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    // Use a long, distinctive string so the stored copy spans more than a
    // single heap granule: obj_addkey_str must allocate room for the NUL and
    // terminate the copy, otherwise obj_readkey_str's strlen() over-reads.
    const char *src = "cardinal-semicolon-objstore-roundtrip";
    TEST_CHECK_EQ_U(ctx, obj_addkey_str(TEST_PATH, "s", src), CS_OK);

    char buf[64];
    // Pre-fill with non-zero so a read path that fails to write the NUL
    // terminator is actually caught (zero-filling would mask it).
    memset(buf, 0xFF, sizeof(buf));
    size_t len = sizeof(buf);
    TEST_CHECK_EQ_U(ctx, obj_readkey_str(TEST_PATH, "s", buf, &len), CS_OK);
    // Exact length proves the stored string is NUL-terminated (a bare strlen on
    // an unterminated copy would return a garbage length).
    TEST_CHECK_EQ_U(ctx, len, strlen(src));
    TEST_CHECK(ctx, buf[strlen(src)] == '\0');
    TEST_CHECK(ctx, strcmp(buf, src) == 0);

    drop_scratch(ctx);
}

// ptr key add + read round-trip.
static void test_key_ptr(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    uintptr_t p = (uintptr_t)0x1000;
    TEST_CHECK_EQ_U(ctx, obj_addkey_ptr(TEST_PATH, "p", p), CS_OK);

    uintptr_t out = 0;
    TEST_CHECK_EQ_U(ctx, obj_readkey_ptr(TEST_PATH, "p", &out), CS_OK);
    TEST_CHECK_EQ_U(ctx, out, p);

    drop_scratch(ctx);
}

// obj_writekey_uint updates an existing key in place.
static void test_writekey_uint(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "u", 1), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_writekey_uint(TEST_PATH, "u", 99), CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, obj_readkey_uint(TEST_PATH, "u", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, v, 99);

    // Writing a key that does not exist reports CS_DNE.
    TEST_CHECK_EQ_U(ctx, obj_writekey_uint(TEST_PATH, "nope", 1), CS_DNE);

    drop_scratch(ctx);
}

// obj_lock / obj_islocked / obj_unlock on a directory handle.
static void test_lock_unlock(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    dir_t dir = NULL;
    TEST_CHECK_EQ_U(ctx, obj_getdirectory(TEST_PATH, &dir), CS_OK);
    TEST_CHECK(ctx, dir != NULL);

    bool locked = true;
    TEST_CHECK_EQ_U(ctx, obj_islocked(dir, &locked), CS_OK);
    TEST_CHECK(ctx, locked == false);

    TEST_CHECK_EQ_U(ctx, obj_lock(dir), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_islocked(dir, &locked), CS_OK);
    TEST_CHECK(ctx, locked == true);

    TEST_CHECK_EQ_U(ctx, obj_unlock(dir), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_islocked(dir, &locked), CS_OK);
    TEST_CHECK(ctx, locked == false);

    drop_scratch(ctx);
}

// obj_getdirectory + obj_next: iterate the entries we added and confirm each
// key name is seen exactly once.
static void test_iteration(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "a", 1), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "b", 2), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "c", 3), CS_OK);

    dir_t dir = NULL;
    TEST_CHECK_EQ_U(ctx, obj_getdirectory(TEST_PATH, &dir), CS_OK);

    bool seen_a = false, seen_b = false, seen_c = false;
    int count = 0;
    dir_t cur = dir;
    while (obj_next(&cur) == CS_OK) {
        char key[256];
        memset(key, 0, sizeof(key));
        if (obj_readlocal_key(cur, key) == CS_OK) {
            count++;
            if (strcmp(key, "a") == 0) seen_a = true;
            if (strcmp(key, "b") == 0) seen_b = true;
            if (strcmp(key, "c") == 0) seen_c = true;
        }
    }

    TEST_CHECK_EQ_U(ctx, count, 3);
    TEST_CHECK(ctx, seen_a);
    TEST_CHECK(ctx, seen_b);
    TEST_CHECK(ctx, seen_c);

    drop_scratch(ctx);
}

// obj_removekey removes an existing key; reading it afterward reports CS_DNE.
static void test_removekey(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "u", 7), CS_OK);

    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, obj_readkey_uint(TEST_PATH, "u", &v), CS_OK);

    TEST_CHECK_EQ_U(ctx, obj_removekey(TEST_PATH, "u"), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_readkey_uint(TEST_PATH, "u", &v), CS_DNE);

    drop_scratch(ctx);
}

// obj_removedirectory cleans up: after removal the path can no longer be walked.
static void test_removedirectory(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    dir_t dir = NULL;
    TEST_CHECK_EQ_U(ctx, obj_getdirectory(TEST_PATH, &dir), CS_OK);

    TEST_CHECK_EQ_U(ctx, obj_removedirectory(TEST_ROOT, TEST_DIR), CS_OK);

    // The directory is gone; walking the path now fails.
    TEST_CHECK_EQ_U(ctx, obj_getdirectory(TEST_PATH, &dir), CS_DNE);
}

// Walk the parent directory's entries and return the handle for the entry whose
// key matches `name`, or NULL if not present. obj_lock/obj_removekey operate on
// the *entry itself*, and a scalar key cannot be reached via obj_getdirectory
// (kvs_walk_path dereferences each component as a child and would PANIC on a
// non-directory), so iteration is the only way to obtain a key's handle.
static dir_t find_entry(const char *path, const char *name) {
    dir_t dir = NULL;
    if (obj_getdirectory(path, &dir) != CS_OK)
        return NULL;

    dir_t cur = dir;
    while (obj_next(&cur) == CS_OK) {
        char key[256];
        memset(key, 0, sizeof(key));
        if (obj_readlocal_key(cur, key) == CS_OK && strcmp(key, name) == 0)
            return cur;
    }
    return NULL;
}

// Lock contract around removal: obj_removekey/obj_removedirectory check whether
// the *target entry* is locked and, if so, silently skip the removal while still
// returning CS_OK. This pins that contract: a removal on a locked entry is a
// no-op (entry still present), and removal only takes effect once unlocked.
static void test_remove_locked_suppressed(test_ctx_t *ctx) {
    if (!make_scratch(ctx))
        return;

    // --- Scalar key: lock suppresses obj_removekey ---
    TEST_CHECK_EQ_U(ctx, obj_addkey_uint(TEST_PATH, "k", 42), CS_OK);

    dir_t key = find_entry(TEST_PATH, "k");
    TEST_CHECK(ctx, key != NULL);
    if (key != NULL)
        TEST_CHECK_EQ_U(ctx, obj_lock(key), CS_OK);

    // Production returns CS_OK but performs NO removal while the entry is locked.
    TEST_CHECK_EQ_U(ctx, obj_removekey(TEST_PATH, "k"), CS_OK);

    // The key must still be present: the lock suppressed the removal.
    uint64_t v = 0;
    TEST_CHECK_MSG(ctx, obj_readkey_uint(TEST_PATH, "k", &v) == CS_OK,
                   "removekey on a locked key must be a no-op");
    TEST_CHECK_EQ_U(ctx, v, 42);

    // Unlock, then removal must actually take effect.
    if (key != NULL)
        TEST_CHECK_EQ_U(ctx, obj_unlock(key), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_removekey(TEST_PATH, "k"), CS_OK);
    TEST_CHECK_MSG(ctx, obj_readkey_uint(TEST_PATH, "k", &v) == CS_DNE,
                   "removekey after unlock must remove the key");

    // --- Subdirectory: lock suppresses obj_removedirectory ---
    // (obj_removedirectory is obj_removekey, so the same lock check applies.)
    TEST_CHECK_EQ_U(ctx, obj_createdirectory(TEST_PATH, "sub"), CS_OK);

    dir_t sub = find_entry(TEST_PATH, "sub");
    TEST_CHECK(ctx, sub != NULL);
    if (sub != NULL)
        TEST_CHECK_EQ_U(ctx, obj_lock(sub), CS_OK);

    TEST_CHECK_EQ_U(ctx, obj_removedirectory(TEST_PATH, "sub"), CS_OK);

    // Re-creating must report CS_EXISTS, proving the locked dir was not removed.
    TEST_CHECK_MSG(ctx, obj_createdirectory(TEST_PATH, "sub") == CS_EXISTS,
                   "removedirectory on a locked dir must be a no-op");

    // Unlock, then removal must actually take effect (re-create now succeeds).
    if (sub != NULL)
        TEST_CHECK_EQ_U(ctx, obj_unlock(sub), CS_OK);
    TEST_CHECK_EQ_U(ctx, obj_removedirectory(TEST_PATH, "sub"), CS_OK);
    TEST_CHECK_MSG(ctx, obj_createdirectory(TEST_PATH, "sub") == CS_OK,
                   "removedirectory after unlock must remove the dir");

    drop_scratch(ctx);
}

#define REGISTER(nm, func)                                          \
    do {                                                            \
        test_def_t t = {                                            \
            .suite = "SysObj", .name = (nm), .fn = (func),          \
            .run = TEST_RUN_INLINE, .flags = TEST_FLAG_NONE,        \
        };                                                          \
        test_register(&t);                                          \
    } while (0)

void sysobj_register_tests(void) {
    if (!test_mode_active())
        return;

    REGISTER("createdirectory", test_createdirectory);
    REGISTER("key_uint", test_key_uint);
    REGISTER("key_int", test_key_int);
    REGISTER("key_bool", test_key_bool);
    REGISTER("key_str", test_key_str);
    REGISTER("key_ptr", test_key_ptr);
    REGISTER("writekey_uint", test_writekey_uint);
    REGISTER("lock_unlock", test_lock_unlock);
    REGISTER("iteration", test_iteration);
    REGISTER("removekey", test_removekey);
    REGISTER("removedirectory", test_removedirectory);
    REGISTER("remove_locked_suppressed", test_remove_locked_suppressed);
}
