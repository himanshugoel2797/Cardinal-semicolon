// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include "test_framework.h"

int g_checks_run = 0;
int g_checks_failed = 0;

int main(void) {
    printf("Cardinal; host unit tests\n");
    printf("-------------------------\n");

    test_checksum();
    test_sha256();
    test_hmac();

    printf("-------------------------\n");
    printf("%d checks, %d failed\n", g_checks_run, g_checks_failed);
    return g_checks_failed ? 1 : 0;
}
