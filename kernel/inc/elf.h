// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _CARDINAL_ELF_H_
#define _CARDINAL_ELF_H_

#include <stddef.h>
#include <elf_types.h>

int elf_installkernelsymbols();
void *elf_resolvefunction(const char *name);
int elf_load(void *elf, size_t elf_len, int (**entry_point)());

#endif
