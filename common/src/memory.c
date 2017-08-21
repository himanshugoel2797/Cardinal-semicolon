#include "stddef.h"
#include "string.h"

void *WEAK memset(void *s, int c, size_t n) {
    unsigned long int c8 = c & 0xFF;
    unsigned long int c32 = (c8 << 24) | (c8 << 16) | (c8 << 8) | (c8);
    unsigned long int c64 = (c32 << 32) | (c32);

    unsigned char *s8 = (unsigned char *)s;
    while (n % 8 > 0) {
        *s8 = c8;
        s8++;
        n--;
    }

    unsigned long int *s64 = (unsigned long int *)s8;
    while (n > 0) {
        *s64 = c64;
        s64++;
        n -= 8;
    }

    return s;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    unsigned char *s = (unsigned char *)src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];

    return dest;
}

size_t strnlen(const char *string, size_t maxLen) {
    if (string == NULL)
        return 0;

    size_t n = 0;
    while (string[n] != 0 && n < maxLen)
        n++;

    return n;
}