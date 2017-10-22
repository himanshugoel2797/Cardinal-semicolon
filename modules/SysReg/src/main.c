#include <types.h>

int print_str(const char *s) {
  while (*s != 0)
    __asm__("outb %1, %0" ::"dN"((uint16_t)0x3f8), "a"(*(s++)));
  return 0;
}

int _start(int a) { print_str("TEST"); return -a; }