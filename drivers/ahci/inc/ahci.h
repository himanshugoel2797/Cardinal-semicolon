// Copyright (c) 2018 hgoel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_DRIVERS_AHCI_H
#define CARDINAL_DRIVERS_AHCI_H

#include <stddef.h>
#include <stdint.h>

//DMA buffer sizes
#define FIS_SIZE 256
#define CMD_BUF_SIZE 1024

//Register offsets
#define HBA_GHC 0x4
#define HBA_IS 0x8
#define HBA_PI 0xC
#define HBA_BOHC 0x28
#define HBA_PxCMD(x) (0x118 + 0x80 * x)
#define HBA_PxSERR(x) (0x130 + 0x80 * x)
#define HBA_PxTFD(x) (0x120 + 0x80 * x)
#define HBA_PxSSTS(x) (0x128 + 0x80 * x)
#define HBA_PxCLB(x) (0x100 + 0x80 * x)
#define HBA_PxCLBU(x) (0x104 + 0x80 * x)
#define HBA_PxFB(x) (0x108 + 0x80 * x)
#define HBA_PxFBU(x) (0x10C + 0x80 * x)
#define HBA_PxIS(x) (0x110 + 0x80 * x)
#define HBA_PxIE(x) (0x114 + 0x80 * x)
#define HBA_PxSACT(x) (0x134 + 0x80 * x)
#define HBA_PxCI(x) (0x138 + 0x80 * x)

//Register bits and masks
#define HBA_PxCMD_ST (1 << 0)
#define HBA_PxCMD_CR (1 << 15)
#define HBA_PxCMD_FR (1 << 14)
#define HBA_PxCMD_FRE (1 << 4)

#define HBA_PxTFD_BSY (1 << 7)
#define HBA_PxTFD_DRQ (1 << 3)

#define HBA_PxIS_DHRS (1 << 0)
#define HBA_PxIS_DSS (1 << 2)
#define HBA_PxIS_DPS (1 << 5)

#define HBA_PxSSTS_DET_MASK 0xF

typedef enum
{
    ATA_CMD_READ_DMA_EXT = 0x25
} ATA_CMD;

typedef enum
{
    fis_type_RegisterH2D = 0x27,
    fis_type_RegisterD2H = 0x34,
    fis_type_DMAActivate = 0x39,
    fis_type_DMASetup = 0x41,
    fis_type_Data = 0x46,
} fis_type_t;

typedef struct
{
    uint8_t fisType;
    struct
    {
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

typedef struct
{

} register_d2h_fis_t;

typedef struct
{

} dma_setup_fis_t;

typedef struct
{

} pio_setup_fis_t;

typedef struct
{

} d2h_fis_t;

typedef struct
{

} sdbs_fis_t;

typedef struct
{
    dma_setup_fis_t dma;
    uint8_t rsv0[8];
    pio_setup_fis_t ps;
    uint8_t rsv1[12];
    d2h_fis_t d2h;
    uint8_t rsv2[4];
    sdbs_fis_t sdbs;
} fis_layout_t;

typedef struct
{
    uint16_t cfl : 5;
    uint16_t atapi : 1;
    uint16_t write : 1;
    uint16_t prefetch : 1;
    uint16_t reset : 1;
    uint16_t bist : 1;
    uint16_t dc : 6;
} ahci_cmd_info_t;

typedef struct
{
    ahci_cmd_info_t info;
    uint16_t PRDTL;
    uint32_t byteCount;
    uint32_t commandTableBaseAddress;
    uint32_t commandTableBaseAddressUpper;
    uint32_t rsv[4];
} ahci_cmd_t;

typedef struct
{
    uint32_t baseAddress;
    uint32_t baseAddressUpper;
    uint32_t rsv0;
    union {
        uint32_t byteCount : 22;
        uint32_t rsv1 : 9;
        uint32_t intCompletion : 1;
    };
} ahci_prdt_t;

typedef struct
{
    uint8_t cmd_fis[64];
    uint8_t atapiCMD[16];
    uint8_t rsv[48];
    ahci_prdt_t prdt[128];
} ahci_cmdtable_t;

typedef struct
{
    uint64_t phys_addr;
    uintptr_t virt_addr;
} ahci_dma_addr_t;

typedef struct ahci_instance
{
    union {
        uint8_t *cfg8;
        uintptr_t cfg;
    };
    int interrupt_vec;
    uint32_t activeDevices;
    uint32_t implPortCnt;
    uint32_t activeCmdBits[32];
    uint32_t finishedCmdBits[32];
    int lock;

    ahci_dma_addr_t port_dma;

    struct ahci_instance *next;
} ahci_instance_t;

PRIVATE void ahci_write8(ahci_instance_t *inst, uint32_t off, uint8_t val);
PRIVATE void ahci_write16(ahci_instance_t *inst, uint32_t off, uint16_t val);
PRIVATE void ahci_write32(ahci_instance_t *inst, uint32_t off, uint32_t val);

PRIVATE uint8_t ahci_read8(ahci_instance_t *inst, uint32_t off);
PRIVATE uint16_t ahci_read16(ahci_instance_t *inst, uint32_t off);
PRIVATE uint32_t ahci_read32(ahci_instance_t *inst, uint32_t off);

PRIVATE void ahci_resethba(ahci_instance_t *inst);
PRIVATE uint32_t ahci_readports(ahci_instance_t *inst);
PRIVATE void ahci_obtainownership(ahci_instance_t *inst);
PRIVATE void ahci_reportawareness(ahci_instance_t *inst);
PRIVATE int ahci_initializeport(ahci_instance_t *inst, int index);
PRIVATE int ahci_getcmdslot(ahci_instance_t *inst, int index);
PRIVATE int ahci_readdev(ahci_instance_t *inst, int index, uint64_t loc, void *addr, uint32_t len);

#endif