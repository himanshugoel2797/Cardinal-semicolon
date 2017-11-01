/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <types.h>

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