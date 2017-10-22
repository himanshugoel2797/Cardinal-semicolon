// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_TAR_H
#define CARDINAL_TAR_H

#include <stdint.h>
#include <stddef.h>

bool
Initrd_GetFile(const char *file,
               void **loc,
               size_t *size);

#endif