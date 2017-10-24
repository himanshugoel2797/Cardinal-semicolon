#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

#include "boot_information.h"
#include "load_script.h"
#include "symbol_db.h"

static char priv_s[2048];
int WEAK debug_handle_trap() {
  const char *p = priv_s;
  print_str(p);
  __asm__("cli\n\thlt");
  return 0;
}
int WEAK print_str(const char *s) {
  while (*s != 0)
    __asm__ volatile("outb %1, %0" ::"dN"((uint16_t)0x3f8), "a"(*(s++)));
  return 0;
}

void WEAK set_trap_str(const char *s) { strncpy(priv_s, s, 2048); }

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

  if (param == NULL)
    PANIC("Didn't receive boot parameters!");

  ParseAndSaveBootInformation(param, magic);

  // Fix boot information addresses
  CardinalBootInfo *b_info = GetBootInfo();
  b_info->InitrdStartAddress += 0xffffffff80000000;

  // Initalize and load
  symboldb_init();
  elf_installkernelsymbols();
  loadscript_execute();

  PANIC("KERNEL END REACHED.");
  while (1)
    ;
  return 0;
}