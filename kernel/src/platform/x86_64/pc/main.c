#include <stddef.h>
#include <stdint.h>
#include <types.h>

int debug_handle_trap() { return 0; }

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

  if (param == NULL)
    return -1;

  if (magic == 0)
    return -2;

  // Initialize linear memory allocator
  // Initialize crypto functions
  // Retrieve builtin SysReg module

  while (1)
    ;
  return 0;
}