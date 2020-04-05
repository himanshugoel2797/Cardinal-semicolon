// Copyright (c) 2020 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_IGFX_GTT_H
#define CARDINALSEMI_IGFX_GTT_H

#include "stdint.h"
#include "stddef.h"
#include "devices.h"

void igfx_gtt_init(igfx_dev_state_t *driver);

uint64_t igfx_gtt_alloc(size_t sz);

void igfx_gtt_free(uint64_t addr, size_t sz);

#endif