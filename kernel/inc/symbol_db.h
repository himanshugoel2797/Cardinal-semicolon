#ifndef CARDINAL_KERNEL_SYMBOLDB_H
#define CARDINAL_KERNEL_SYMBOLDB_H

#include <stddef.h>
#include <stdint.h>

#include "elf.h"

void symboldb_init();

int symboldb_add(Elf64_Shdr *strhdr, Elf64_Shdr *hdr, Elf64_Sym *symbol);

int symboldb_findmatch(Elf64_Shdr *strhdr, Elf64_Shdr *hdr, Elf64_Sym *sym,
                       Elf64_Shdr **r_hdr, Elf64_Sym **r_sym);

int symboldb_findfunc(const char *str, Elf64_Shdr **r_hdr, Elf64_Sym **r_sym);

#endif