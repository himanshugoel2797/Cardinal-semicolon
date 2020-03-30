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

typedef struct
{
    volatile uint32_t crst : 1;
    uint32_t fcntrl : 1;
    uint32_t rsv0 : 6;
    uint32_t unsol : 1;
    uint32_t rsv1 : 23;
} hdaudio_gctl_t;

typedef struct
{
    uint32_t sie : 30;
    uint32_t cie : 1;
    uint32_t gie : 1;
} hdaudio_intctl_t;

typedef struct
{
    uint32_t sis : 30;
    uint32_t cis : 1;
    uint32_t gis : 1;
} hdaudio_intsts_t;

typedef struct
{
    uint8_t cmeie : 1;
    volatile uint8_t corbrun : 1;
    uint8_t rsv0 : 6;
} hdaudio_corb_ctl_t;

typedef struct
{
    uint8_t cmei : 1;
    uint8_t rsv0 : 7;
} hdaudio_corb_sts_t;

typedef struct
{
    uint8_t sz : 2;
    uint8_t rsv0 : 2;
    uint8_t szcap : 4;
} hdaudio_corb_sz_t;

typedef struct PACKED
{
    uint32_t lower_base;
    uint32_t upper_base;
    volatile uint16_t wp;
    volatile uint16_t rp;
    hdaudio_corb_ctl_t ctl;
    hdaudio_corb_sts_t sts;
    hdaudio_corb_sz_t sz;
} hdaudio_corb_t;

typedef struct
{
    uint8_t rintctl : 1;
    uint8_t dmaen : 1;
    uint8_t oic : 1;
} hdaudio_rirb_ctl_t;

typedef struct
{
    uint8_t rintfl : 1;
    uint8_t rsv0 : 1;
    uint8_t ois : 1;
} hdaudio_rirb_sts_t;

typedef struct
{
    uint8_t sz : 2;
    uint8_t rsv0 : 2;
    uint8_t szcap : 4;
} hdaudio_rirb_sz_t;

typedef struct PACKED
{
    uint32_t lower_base;
    uint32_t upper_base;
    uint16_t wp;
    uint16_t intcnt;
    hdaudio_rirb_ctl_t ctl;
    hdaudio_rirb_sts_t sts;
    hdaudio_rirb_sz_t sz;
} hdaudio_rirb_t;

typedef struct PACKED
{
    hdaudio_gcap_t gcap;
    uint8_t vmin;
    uint8_t vmaj;
    uint16_t outpay;
    uint16_t inpay;
    hdaudio_gctl_t gctl;
    uint16_t wakeen;
    uint16_t statests;
    uint16_t gsts;
    uint8_t rsv0[6];
    uint16_t outstrmpay;
    uint16_t instrmpay;
    uint8_t rsv1[4];
    hdaudio_intctl_t intctl;
    hdaudio_intsts_t intsts;
    uint8_t rsv2[8];
    uint32_t wallclock;
    uint8_t rsv3[4];
    uint32_t ssync;
    uint8_t rsv4[4];
    hdaudio_corb_t corb;
    uint8_t rsv5;
    hdaudio_rirb_t rirb;
    uint32_t dplbase;
    uint32_t dpubase;
} hdaudio_regs_t;

#endif