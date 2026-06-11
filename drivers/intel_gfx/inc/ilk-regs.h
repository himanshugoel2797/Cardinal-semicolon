// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Ironlake (Gen5) display register definitions, ported from the original
// Cardinal "ihd" driver. Offsets are absolute from the start of the
// MMIO/GTTMMADR BAR (BAR0), matching the igfx_read32/igfx_write32 model.

#ifndef CARDINALSEMI_IGFX_ILK_REGS_H
#define CARDINALSEMI_IGFX_ILK_REGS_H

#include "stdint.h"

// --- Pipes ---
#define IGFX_ILK_PIPE_CONF(x) (0x70008 + ((x) * 0x1000))
#define IGFX_ILK_PIPE_CONF_ENABLE (1u << 31)
#define IGFX_ILK_PIPE_CONF_STATE (1u << 30)
// Pipe source size: [27:16] width-1, [11:0] height-1
#define IGFX_ILK_PIPE_SRC(x) (0x6001C + ((x) * 0x1000))

// --- Primary display planes ---
#define IGFX_ILK_DSP_CTRL(x) (0x70180 + ((x) * 0x1000))
#define IGFX_ILK_DSP_LINOFF(x) (0x70184 + ((x) * 0x1000))
#define IGFX_ILK_DSP_STRIDE(x) (0x70188 + ((x) * 0x1000))
#define IGFX_ILK_DSP_SURF(x) (0x7019C + ((x) * 0x1000))

#define IGFX_ILK_DSP_CTRL_ENABLE (1u << 31)
#define IGFX_ILK_DSP_CTRL_GAMMA_ENABLE (1u << 30)
#define IGFX_ILK_DSP_CTRL_PIXEL_MODE_OFF 26
#define IGFX_ILK_DSP_CTRL_PIXEL_MODE_MASK 0xF

// Pixel formats (DSP_CTRL bits [29:26])
#define IGFX_ILK_PIXEL_XRGB_8888 0x6
#define IGFX_ILK_PIXEL_XBGR_8888 0xE

// --- Panel power sequencing ---
#define IGFX_ILK_PP_STAT 0xC7200
#define IGFX_ILK_PP_CTRL 0xC7204
#define IGFX_ILK_PP_CTRL_BACKLIGHT (1u << 2)

// --- Backlight PWM ---
#define IGFX_ILK_BLC_PWM_CTL2 0x48250
#define IGFX_ILK_BLC_PWM_CTL2_ENABLE (1u << 31)
#define IGFX_ILK_BLC_PWM_CTL1 0x48254
#define IGFX_ILK_PWM_PCH_CTRL 0xC8250
// PWM modulation frequency: upper 16 bits hold the max brightness period
#define IGFX_ILK_PWM_MOD_FREQ 0xC8254

// --- VGA ---
#define IGFX_ILK_VGA_PLANE_CTRL 0x41000
#define IGFX_ILK_VGA_PLANE_CTRL_DISABLE (1u << 31)

#endif
