#include "load_script.h"
#include "elf.h"
#include "module_def.h"

#include "initrd.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

int loadscript_execute() {

  char *load_script = NULL;
  char name[1024];
  size_t load_len = 0;
  if (!Initrd_GetFile("./loadscript.txt", (void **)&load_script, &load_len))
    PANIC("FAILED TO FIND LOADSCRIPT");

  uintptr_t load_script_end = (uintptr_t)(load_script + load_len);
  while ((uintptr_t)load_script < load_script_end) {

    int mode = -1;

    if (strncmp(load_script, "LOAD:", 5) == 0)
      mode = 0;
    else if (strncmp(load_script, "CALL:", 5) == 0)
      mode = 1;

    load_script += 5;
    const char *end_of_line = strstr(load_script, "\n");
    memset(name, 0, 1024);
    strncpy(name, load_script, (size_t)(end_of_line - load_script));
    load_script += end_of_line - load_script + 1;

    if (mode == 0) {

      print_str("LOAD MODULE:");
      print_str(name);
      print_str("\r\n");

      void *mod_loc = NULL;
      size_t len = 0;
      if (!Initrd_GetFile(name, &mod_loc, &len))
        PANIC("FAILED TO FIND MODULE!");

      // decompress celf's elf section
      ModuleHeader *hdr = (ModuleHeader *)mod_loc;

      int (*entry_pt)() = NULL;
      if (elf_load(hdr->data, hdr->uncompressed_len, &entry_pt))
        PANIC("ELF LOAD FAILED");

      print_str("LOADED\r\n");

      //__asm__("hlt" ::"a"(entry_pt));
      entry_pt();
    } else if (mode == 1) {
      print_str("CALL FUNCTION:");
      print_str(name);
      print_str("\r\n");

      int (*entry_pt)() = (int (*)())elf_resolvefunction(name);
      entry_pt();
    } else if (mode == -1)
      break;
  }

  return 0;
}