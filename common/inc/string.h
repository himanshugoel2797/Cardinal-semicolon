#ifndef _STRING_H_
#define _STRING_H_

#include "stddef.h"
#include "stdint.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void *memset(void *s, int c, size_t n);

void *memcpy(void *restrict dest, const void *restrict src, size_t n);

int memcmp(const void *__s1, const void *__s2, size_t __n) PURE;

void *memmove(void *restrict dest, const void *restrict src, size_t n);

size_t strlen(const char *s) PURE;

size_t strnlen(const char *string, size_t maxlen) PURE;

int strncmp(const char *s1, const char *s2, size_t n) PURE;

int strcmp(const char *s1, const char *s2) PURE;

char *strncpy(char *restrict dest, const char *restrict src, size_t len);

char *strncat(char *restrict dest, const char *restrict src, size_t count);

const char *strstr(const char *haystack, const char *needle) PURE;

const char *strchr(const char *__s, int __c) PURE;

const char *strrchr(const char *__s, int __c) PURE;

#ifdef __cplusplus
}
#endif

#endif