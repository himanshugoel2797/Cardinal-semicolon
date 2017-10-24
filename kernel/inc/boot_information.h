// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_PARSE_BOOT_INFO_H
#define CARDINAL_PARSE_BOOT_INFO_H

#include <cardinal/boot_info.h>
#include <stdint.h>
#include <types.h>

void ParseAndSaveBootInformation(void *boot_info NONNULL, uint32_t magic);

CardinalBootInfo *GetBootInfo(void);

#endif