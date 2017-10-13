// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_BOOTSTRAP_ALLOC_H
#define CARDINAL_BOOTSTRAP_ALLOC_H

#include <stddef.h>
#include <stdint.h>

void *bootstrap_malloc(size_t s);
void bootstrap_free(void *mem, size_t s);

#endif