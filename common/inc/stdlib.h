#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

void *malloc(size_t size);

void free(void *ptr);

char *itoa(int val, char *dst, int base);

char *ltoa(long long val, char *dst, int base);

int atoi(const char * ptr, int base);

#endif