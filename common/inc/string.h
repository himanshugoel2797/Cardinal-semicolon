#ifndef _STRING_H_
#define _STRING_H_

#include "stddef.h"
#include "stdint.h"

void *memset(void *s, int c, size_t n);

void *memcpy(void *restrict dest, const void *restrict src, size_t n);

size_t strnlen(const char *string, size_t maxlen);

#endif