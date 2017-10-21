#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

#include "boot_information.h"
#include "bootstrap_alloc.h"
#include "elf.h"
#include "initrd.h"
#include "miniz.h"
#include "module_def.h"

static char priv_s[2048];
int debug_handle_trap() {
  const char *p = priv_s;
  while (*p != 0)
    __asm__("outb %1, %0" ::"dN"((uint16_t)0x3f8), "a"(*(p++)));
  __asm__("cli\n\thlt");
  return 0;
}
int print_str(const char *s) {
  while (*s != 0)
    __asm__("outb %1, %0" ::"dN"((uint16_t)0x3f8), "a"(*(s++)));
  return 0;
}

void set_trap_str(const char *s) { strncpy(priv_s, s, 2048); }

int Elf64Load(void *elf, size_t elf_len);

tinfl_decompressor g_inflator;

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

  if (param == NULL)
    PANIC("Didn't receive boot parameters!");

  ParseAndSaveBootInformation(param, magic);

  CardinalBootInfo *b_info = GetBootInfo();
  b_info->InitrdStartAddress += 0xffffffff80000000;

  // Retrieve modules from initrd and start verification and loading based on
  // the supplied boot script, do this in HAL code

  // bootscript contains LOAD commands and CALL commands
  // LOAD commands tell the kernel to load the module and link it in
  // CALL commands tell the kernel to call a function with a certain NID from
  // the specified module name

  tinfl_init(&g_inflator);

  void *sysreg_loc = NULL;
  size_t len = 0;
  if (!Initrd_GetFile("./SysReg.celf", &sysreg_loc, &len))
    PANIC("FAILED TO FIND SysReg.celf");

  // decompress celf's elf section
  ModuleHeader *hdr = (ModuleHeader *)sysreg_loc;
  /*size_t u_len = hdr->uncompressed_len, c_len = hdr->elf_len;

  void *dst = bootstrap_malloc(u_len);
  void *next_out = dst;
  tinfl_status status = tinfl_decompress(&g_inflator, hdr->data, &c_len, dst,
  dst, &u_len, TINFL_FLAG_PARSE_ZLIB_HEADER |
  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  */

  if (Elf64Load(hdr->data, hdr->uncompressed_len))
    PANIC("ELF LOAD FAILED");

  PANIC("KERNEL END REACHED.");
  while (1)
    ;
  return 0;
}