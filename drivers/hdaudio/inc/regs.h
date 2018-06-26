// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_HDAUDIO_REGS_H
#define CARDINAL_DRIVERS_HDAUDIO_REGS_H

#include <stdint.h>

typedef union {
    uint16_t is64bit : 1;
    uint16_t nsdo : 2;
    uint16_t bss : 5;
    uint16_t iss : 4;
    uint16_t oss : 4;
} hdaudio_gcap_t;

typedef struct {
    uint32_t crst : 1;
    uint32_t fcntrl : 1;
    uint32_t rsv0 : 6;
    uint32_t unsol : 1;
    uint32_t rsv1 : 23;
} hdaudio_gctl_t;

typedef struct {
    uint32_t sie : 30;
    uint32_t cie : 1;
    uint32_t gie : 1;
} hdaudio_intctl_t;

typedef struct {
    hdaudio_gcap_t gcap;
    uint8_t vmin;
    uint8_t vmaj;
    uint16_t outpay;
    uint16_t inpay;
    hdaudio_gctl_t gctl;
    uint16_t sdiwen;
    uint16_t sdiwake;
    uint16_t gsts;
    uint8_t rsv0[6];
    uint16_t outstrmpay;
    uint16_t instrmpay;
    uint8_t rsv1[4];
    hdaudio_intctl_t intctl;
} hdaudio_regs_t;

#endif