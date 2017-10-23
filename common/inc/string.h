#ifndef _STRING_H_
#define _STRING_H_

#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

void *memset(void *s, int c, size_t n);

void *memcpy(void *restrict dest, const void *restrict src, size_t n);

size_t strlen(const char *s);

size_t strnlen(const char *string, size_t maxlen);

int strncmp(const char *s1, const char *s2, size_t n);

int strcmp(const char *s1, const char *s2);

char *strncpy(char *restrict dest, const char *restrict src, size_t len);

char *strncat(char *restrict dest, const char *restrict src, size_t count);

#ifdef __cplusplus
}
#endif

#endif