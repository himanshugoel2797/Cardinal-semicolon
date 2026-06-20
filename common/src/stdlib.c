/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

// calloc over the module allocator (malloc/free live in SysMemory): allocate
// nmemb*size zeroed bytes, with an overflow guard on the product. WEAK so a module
// may override; the shader tier (libs/lisp_shader) relies on this in-OS.
void *WEAK calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
        return NULL;  // multiplication would overflow
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

char *WEAK itoa(int val, char *dst, int base) {
    char *iter = dst;
    int len = 0, start = 0;
    uint32_t tmp = (uint32_t)val;

    if (base == 0)
        return NULL;

    if (dst == NULL)
        return NULL;

    if (val == 0) {
        dst[0] = '0';
        dst[1] = 0;
        return dst;
    }

    if (val < 0 && base == 10)
        tmp = (uint32_t)-val;

    while (tmp != 0) {
        char v = tmp % base;
        char c = 0;

        if (v <= 9)
            c = '0' + v;
        else if (v <= 35)
            c = 'a' + (v - 10);

        *(iter++) = c;
        *iter = 0;
        len++;

        tmp = tmp / base;
    }

    if (val < 0 && base == 10) {
        *(iter++) = '-';
        *iter = 0;
        len++;
    }

    while (start < len) {
        char t = dst[start];
        dst[start] = dst[len - 1];
        dst[len - 1] = t;

        len--;
        start++;
    }

    return dst;
}

char* WEAK ltoa(long long val, char *dst, int base) {
    char *iter = dst;
    int len = 0, start = 0;
    uint64_t tmp = (uint64_t)val;

    if (base == 0)
        return NULL;

    if (dst == NULL)
        return NULL;

    if (val == 0) {
        dst[0] = '0';
        dst[1] = 0;
        return dst;
    }

    if (val < 0 && base == 10)
        tmp = (uint64_t)-val;

    while (tmp != 0) {
        char v = tmp % base;
        char c = 0;

        if (v <= 9)
            c = '0' + v;
        else if (v <= 35)
            c = 'a' + (v - 10);

        *(iter++) = c;
        *iter = 0;
        len++;

        tmp = tmp / base;
    }

    if (val < 0 && base == 10) {
        *(iter++) = '-';
        *iter = 0;
        len++;
    }

    while (start < len) {
        char t = dst[start];
        dst[start] = dst[len - 1];
        dst[len - 1] = t;

        len--;
        start++;
    }

    return dst;
}

int WEAK atoi(const char * ptr, int base) {

    if(base == 16) {
        int val = 0;
        int digit = 0;
        while(true) {
            if(*ptr >= '0' && *ptr <= '9') {
                val |= (*ptr - '0');
            } else if(*ptr >= 'a' && *ptr <= 'f') {
                val |= (*ptr - 'a') + 10;
            } else if(*ptr >= 'A' && *ptr <= 'F') {
                val |= (*ptr - 'A') + 10;
            } else {
                val = val >> 4;
                return val;
            }

            ptr++;
            val = val << 4;
        }
    } else
        return -1;
}