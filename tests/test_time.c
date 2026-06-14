// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// Tests for common/src/time.c (gmtime / strftime). That source is compiled into
// this test with the symbols renamed (gmtime->cardinal_gmtime,
// strftime->cardinal_strftime via -D, see tests/CMakeLists.txt) so it does not
// collide with the host libc. We include the kernel's own <time.h> by relative
// path to get its struct tm / time_t without putting common/inc on the global
// header search path (which would shadow the host string.h/stdio.h the other
// tests need).

#include "test_framework.h"
#include "../common/inc/time.h"

struct tm *cardinal_gmtime(const time_t *);
size_t cardinal_strftime(char *restrict, size_t, const char *restrict,
                         const struct tm *restrict);

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void test_time(void) {
    printf("[time]\n");

    // Epoch: 1970-01-01 00:00:00 UTC, a Thursday.
    time_t t0 = 0;
    struct tm *tm0 = cardinal_gmtime(&t0);
    CHECK_EQ_U(tm0->tm_year + 1900, 1970);
    CHECK_EQ_U(tm0->tm_mon, 0);
    CHECK_EQ_U(tm0->tm_mday, 1);
    CHECK_EQ_U(tm0->tm_hour, 0);
    CHECK_EQ_U(tm0->tm_min, 0);
    CHECK_EQ_U(tm0->tm_sec, 0);
    CHECK_EQ_U(tm0->tm_wday, 4);  // Thursday
    CHECK_EQ_U(tm0->tm_yday, 0);

    // The Unix billennium: 1_000_000_000 -> 2001-09-09 01:46:40 UTC, a Sunday.
    // gmtime returns a pointer to a single static struct tm, so take a copy
    // before any later gmtime call clobbers it (the strftime tests below reuse
    // it).
    time_t tb = 1000000000;
    struct tm tmb = *cardinal_gmtime(&tb);
    CHECK_EQ_U(tmb.tm_year + 1900, 2001);
    CHECK_EQ_U(tmb.tm_mon, 8);   // September
    CHECK_EQ_U(tmb.tm_mday, 9);
    CHECK_EQ_U(tmb.tm_hour, 1);
    CHECK_EQ_U(tmb.tm_min, 46);
    CHECK_EQ_U(tmb.tm_sec, 40);
    CHECK_EQ_U(tmb.tm_wday, 0);    // Sunday
    CHECK_EQ_U(tmb.tm_yday, 251);  // 0-based day of year

    // A leap day: 951_782_400 -> 2000-02-29 00:00:00 UTC, a Tuesday.
    time_t tl = 951782400;
    struct tm *tml = cardinal_gmtime(&tl);
    CHECK_EQ_U(tml->tm_year + 1900, 2000);
    CHECK_EQ_U(tml->tm_mon, 1);   // February
    CHECK_EQ_U(tml->tm_mday, 29);
    CHECK_EQ_U(tml->tm_wday, 2);  // Tuesday
    CHECK_EQ_U(tml->tm_yday, 59);

    // strftime against the billennium tm.
    char buf[64];
    size_t n;

    n = cardinal_strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmb);
    CHECK(streq(buf, "2001-09-09 01:46:40"));
    CHECK_EQ_U(n, 19);

    n = cardinal_strftime(buf, sizeof(buf), "%a %b %d, %Y", &tmb);
    CHECK(streq(buf, "Sun Sep 09, 2001"));

    n = cardinal_strftime(buf, sizeof(buf), "%I?%p j=%j 100%%", &tmb);
    // %I is unsupported -> emitted verbatim as "%I".
    CHECK(streq(buf, "%I?AM j=252 100%"));

    // Truncation: a buffer that cannot hold the result returns 0.
    char small[4];
    n = cardinal_strftime(small, sizeof(small), "%Y-%m-%d", &tmb);
    CHECK_EQ_U(n, 0);

    // Exact-fit boundary: "2001" needs 4 chars + NUL = 5.
    char five[5];
    n = cardinal_strftime(five, sizeof(five), "%Y", &tmb);
    CHECK_EQ_U(n, 4);
    CHECK(streq(five, "2001"));
}
