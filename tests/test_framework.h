// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// A deliberately tiny host-side test framework: no dependencies beyond stdio,
// so it can exercise the freestanding-but-host-compilable leaf code (crypto,
// checksums, libc helpers) without pulling in the kernel.

#ifndef CARDINAL_TEST_FRAMEWORK_H
#define CARDINAL_TEST_FRAMEWORK_H

#include <stdio.h>

extern int g_checks_run;
extern int g_checks_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks_run++;                                                   \
        if (!(cond)) {                                                    \
            g_checks_failed++;                                            \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

#define CHECK_EQ_U(a, b)                                                  \
    do {                                                                  \
        unsigned long long _a = (unsigned long long)(a);                  \
        unsigned long long _b = (unsigned long long)(b);                  \
        g_checks_run++;                                                   \
        if (_a != _b) {                                                   \
            g_checks_failed++;                                            \
            printf("  FAIL %s:%d: %s (0x%llx) != %s (0x%llx)\n",          \
                   __FILE__, __LINE__, #a, _a, #b, _b);                   \
        }                                                                 \
    } while (0)

// Each test group exposes a void(void) entry point invoked by the runner.
void test_sha256(void);
void test_hmac(void);
void test_time(void);

#endif
