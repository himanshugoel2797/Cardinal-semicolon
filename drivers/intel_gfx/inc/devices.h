// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_INTEL_GFX_DEVICES_H
#define CARDINALSEMI_INTEL_GFX_DEVICES_H

#include <stdint.h>

typedef struct {
    char *name;
    uint16_t device_id;
    uintptr_t bar_virt;
} intel_gfx_device_t;

intel_gfx_device_t hsw = {
    .name = "Intel Haswell-ULT Integrated Graphics",
    .device_id = 0x0a16,
};

#endif