#include <stddef.h>
#include <stdint.h>
#include <types.h>

#include "boot_information.h"

int debug_handle_trap() { __asm__ ("cli\n\thlt"); return 0; }

void set_trap_str(const char *s) { s = NULL; }

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

  if (param == NULL)
    PANIC("Didn't receive boot parameters!");

  ParseAndSaveBootInformation(param, magic);

  // Retrieve modules from initrd and start verification and loading based on
  // the supplied boot script, do this in HAL code

  // bootscript contains LOAD commands and CALL commands
  // LOAD commands tell the kernel to load the module and link it in
  // CALL commands tell the kernel to call a function with a certain NID from
  // the specified module name

  while (1)
    ;
  return 0;
}