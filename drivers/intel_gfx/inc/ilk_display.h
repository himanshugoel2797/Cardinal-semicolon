// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_IGFX_ILK_DISPLAY_H
#define CARDINALSEMI_IGFX_ILK_DISPLAY_H

#include "devices.h"

// Bring up the Ironlake display via firmware hand-off and register it with
// CoreDisplay. This reuses the mode the firmware already programmed; it does
// not perform a cold mode-set. See ilk_display.c for details.
void igfx_ilk_init(igfx_dev_state_t *driver);

#endif
