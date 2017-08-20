#include "libc.h"
#include "types.h"

void *memset(void *s, int c, size_t n) WEAK {
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
    *sc = c64;
    sc++;
    n -= 8;
  }

  return s;
}