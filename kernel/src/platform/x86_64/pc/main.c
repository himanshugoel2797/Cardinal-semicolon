#include <stddef.h>
#include <stdint.h>
#include <types.h>

#include "boot_information.h"

int debug_handle_trap() { return 0; }

void set_trap_str(const char *s) { s = NULL; }

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

  if (param == NULL)
    PANIC("Didn't receive boot parameters!");

  ParseAndSaveBootInformation(param, magic);

  // Retrieve modules from initrd and start verification and loading based on
  // the supplied boot script, do this in HAL code

  while (1)
    ;
  return 0;
}