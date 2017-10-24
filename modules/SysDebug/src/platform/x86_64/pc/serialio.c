// TODO: define serial io functions for debugging
// TODO: make load script be a platform specific file

// TODO: first initialize output, make PANIC switch to debug mode
// TODO: debug mode uses available information to allow probing of kernel
// TODO: allows addition of additional debug providers

#include "serialio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERIAL_A ((uint16_t)0x3f8)
#define SERIAL_A_STATUS ((uint16_t)0x3fd)

int kernel_updatetraphandlers();

int init_serial_debug() {
  // kernel_updatetraphandlers();
  return 0;
}

static inline bool serial_outputisready() {
  char s = 0;
  __asm__ volatile("inb %1, %0" : "=r"(s) : "dN"((SERIAL_A_STATUS)));

  return s & 0x20;
}

static inline bool serial_inputisready() {
  char s = 0;
  __asm__ volatile("inb %1, %0" : "=r"(s) : "dN"((SERIAL_A_STATUS)));

  return s & 0x1;
}

static inline void serial_output(char c) {
  while (!serial_outputisready())
    ;
  __asm__ volatile("outb %1, %0" ::"dN"(SERIAL_A), "r"(c));
}

static inline char serial_input() {
  char c = 0;
  while (!serial_inputisready())
    ;
  __asm__ volatile("inb %1, %0" : "=r"(c) : "dN"(SERIAL_A));
  return c;
}

static char priv_s[2048];
int WEAK debug_handle_trap() {
  const char *p = priv_s;
  print_str(p);
  serial_input();
  __asm__("cli\n\thlt");
  return 0;
}
int WEAK print_str(const char *s) {
  while (*s != 0)
    serial_output(*(s++));
  return 0;
}

void WEAK set_trap_str(const char *s) { strncpy(priv_s, s, 2048); }