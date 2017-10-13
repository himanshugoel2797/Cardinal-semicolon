#include <stddef.h>
#include <stdint.h>
#include <types.h>

#include "boot_information.h"
#include "initrd.h"
#include "elf.h"

const char *priv_s;
int debug_handle_trap() { while(*priv_s != 0) __asm__("outb %1, %0" :: "dN"((uint16_t)0x3f8), "a"(*(priv_s++))); __asm__ ("cli\n\thlt"); return 0; }
int print_str(const char *s) { while(*s != 0) __asm__("outb %1, %0" :: "dN"((uint16_t)0x3f8), "a"(*(s++))); return 0; }

void set_trap_str(const char *s) { priv_s = s; }


int Elf64Load(void *elf, size_t elf_len);

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

  void *sysreg_loc = NULL;
  size_t len = 0;
  if(!Initrd_GetFile("./SysReg.celf", &sysreg_loc, &len))
    PANIC("FAILED TO FIND SysReg.celf");

  //decompress celf's elf section

  if(!Elf64Load(sysreg_loc, len))
    PANIC("ELF LOAD FAILED");

  PANIC("KERNEL END REACHED.");
  while (1)
    ;
  return 0;
}