// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_INTEL_WIFI_DEVICES_H
#define CARDINALSEMI_INTEL_WIFI_DEVICES_H

#include <stdint.h>
#include <types.h>

#include "firmware.h"

#define	DEVID_3160_1 0x08b3
#define	DEVID_3160_2 0x08b4
#define	DEVID_3165_1 0x3165
#define	DEVID_3165_2 0x3166
#define	DEVID_3168_1 0x24fb
#define	DEVID_7260_1 0x08b1
#define	DEVID_7260_2 0x08b2
#define	DEVID_7265_1 0x095a
#define	DEVID_7265_2 0x095b
#define	DEVID_8260_1 0x24f3
#define	DEVID_8260_2 0x24f4
#define	DEVID_8265_1 0x24fd

#define FAMILY_7000 1
#define FAMILY_8000 2

typedef struct {
    const char *name;
    const char *fw_file;
    uint16_t devID;
    uint16_t family;
} iwifi_dev_t;

extern iwifi_dev_t iwifi_devices[];

typedef struct {
    uintptr_t paddr;
    uint8_t *vaddr;
} iwifi_addr_t;

typedef struct {
    iwifi_dev_t *device;
    uintptr_t bar_phys;
    uint8_t *bar;

    uint32_t int_mask;
    uint32_t hw_rev;
    uint32_t fw_mem_offset;
    volatile uint8_t fh_tx_int;
    volatile uint8_t rf_kill;
    fw_info_t fw_info;

    iwifi_addr_t tx_sched_mem;
    iwifi_addr_t kw_mem;
    iwifi_addr_t rx_mem;
    iwifi_addr_t tx_mem;
    iwifi_addr_t tx_cmd_mem;
    iwifi_addr_t rx_status_mem;
    iwifi_addr_t rx_bufs_mem;

    void *state;
} iwifi_dev_state_t;

PRIVATE int iwifi_getdevice(uint16_t devID, iwifi_dev_t **dev);

uint64_t iwifi_read64(iwifi_dev_state_t *dev, int off);
uint32_t iwifi_read32(iwifi_dev_state_t *dev, int off);
uint16_t iwifi_read16(iwifi_dev_state_t *dev, int off);
uint8_t iwifi_read8(iwifi_dev_state_t *dev, int off);

void iwifi_write64(iwifi_dev_state_t *dev, int off, uint64_t val);
void iwifi_write32(iwifi_dev_state_t *dev, int off, uint32_t val);
void iwifi_write16(iwifi_dev_state_t *dev, int off, uint16_t val);
void iwifi_write8(iwifi_dev_state_t *dev, int off, uint8_t val);

void iwifi_periph_write32(iwifi_dev_state_t *dev, int off, uint32_t val);
uint32_t iwifi_periph_read32(iwifi_dev_state_t *dev, int off);

#define iwifi_setbits32(dev, off, flags) iwifi_write32(dev, off, iwifi_read32(dev, off) | flags)
#define iwifi_clrbits32(dev, off, flags) iwifi_write32(dev, off, iwifi_read32(dev, off) & ~(flags))

#define iwifi_periph_setbits32(dev, off, flags) iwifi_periph_write32(dev, off, iwifi_periph_read32(dev, off) | flags)
#define iwifi_periph_clrbits32(dev, off, flags) iwifi_periph_write32(dev, off, iwifi_periph_read32(dev, off) & ~(flags))


#endif