// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef _CARDINAL_ELF_H_
#define _CARDINAL_ELF_H_

#include <stddef.h>
#include <elf_types.h>

#include "cs_syscall.h"

int user_elf_load(cs_id task_id, void *elf, size_t elf_len, void (**entry_point)(void *));

#endif
