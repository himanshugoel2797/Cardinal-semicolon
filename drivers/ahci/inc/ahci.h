// Copyright (c) 2018 hgoel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_AHCI_H
#define CARDINAL_DRIVERS_AHCI_H

#include <stdint.h>

typedef enum {
    ATA_CMD_READ_DMA_EXT = 0x25
} ATA_CMD;

typedef enum {
    fis_type_RegisterH2D = 0x27,
    fis_type_RegisterD2H = 0x34,
    fis_type_DMAActivate = 0x39,
    fis_type_DMASetup = 0x41,
    fis_type_Data = 0x46,
} fis_type_t;

typedef struct {
    uint8_t fisType;
    struct {
        uint8_t pm_port : 4;
        uint8_t rsv0 : 3;
        uint8_t cmd : 1;
    };
    uint8_t command;
    uint8_t features_lo;
    uint16_t lba_lo;
    uint8_t lba_mid;
    uint8_t device;
    uint16_t lba_mid_h;
    uint8_t lba_hi;
    uint8_t features_hi;
    uint16_t count;
    uint8_t icc;
    uint8_t control;
    uint32_t rsv1;
} register_h2d_fis_t;

typedef struct {

} register_d2h_fis_t;

typedef struct {

} dma_setup_fis_t;

typedef struct {

} pio_setup_fis_t;

typedef struct {

} d2h_fis_t;

typedef struct {

} sdbs_fis_t;

typedef struct {
    dma_setup_fis_t dma;
    uint8_t rsv0[8];
    pio_setup_fis_t ps;
    uint8_t rsv1[12];
    d2h_fis_t d2h;
    uint8_t rsv2[4];
    sdbs_fis_t sdbs;
} fis_layout_t;

typedef struct {
    uint16_t cfl : 5;
    uint16_t atapi : 1;
    uint16_t write : 1;
    uint16_t prefetch : 1;
    uint16_t reset : 1;
    uint16_t bist : 1;
    uint16_t dc : 6;
} ahci_cmd_info_t;

typedef struct {
    ahci_cmd_info_t info;
    uint16_t PRDTL;
    uint32_t byteCount;
    uint32_t commandTableBaseAddress;
    uint32_t commandTableBaseAddressUpper;
    uint32_t rsv[4];
} ahci_cmd_t;

typedef struct {
    uint32_t baseAddress;
    uint32_t baseAddressUpper;
    uint32_t rsv0;
    union {
        uint32_t byteCount : 22;
        uint32_t rsv1 : 9;
        uint32_t intCompletion : 1;
    };
} ahci_prdt_t;

typedef struct {
    uint8_t cmd_fis[64];
    uint8_t atapiCMD[16];
    uint8_t rsv[48];
    ahci_prdt_t prdt[128];
} ahci_cmdtable_t;

#endif