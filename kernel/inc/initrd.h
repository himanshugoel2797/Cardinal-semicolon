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

// Shadow initrd file `name` with `len` bytes of `data` (copied), so subsequent
// Initrd_GetFile(name) returns it instead of the baked file. Lets any boot file
// be supplied at runtime (e.g. over serial). Returns 0 on success, -1 if full.
int Initrd_AddOverlay(const char *name, const void *data, size_t len);

#endif