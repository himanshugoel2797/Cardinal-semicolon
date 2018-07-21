// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_IWIFI_FIRMWARE_H
#define CARDINALSEMI_IWIFI_FIRMWARE_H

#include <stdint.h>
#include "devices.h"

#define ENABLED_CAPS_LEN 4
#define ENABLED_API_LEN 1
#define MAX_SECTION_COUNT 0x10

typedef struct {
    void *data;
    uintptr_t paddr;
    uint32_t len;
    uint32_t offset;
} fw_section_t;

typedef enum {
    fw_section_regular,
    fw_section_init,
    fw_section_wowlan,
    fw_section_usniffer,
    fw_section_count,
} fw_section_type_t;

typedef struct {
    fw_section_t sections[MAX_SECTION_COUNT];
    uint32_t section_ent;
    uint32_t flow_trigger;
    uint32_t event_trigger;
} fw_section_ent_t;

typedef struct {
    uint32_t probe_max_len;
    uint32_t phy_sku;
    uint32_t paging_sz;
    uint32_t n_scan_channels;
    uint32_t fw_version[3];
    uint32_t flags;
    uint32_t num_of_cpu;
    fw_section_ent_t section_entries[fw_section_count];
    uint32_t enabled_caps[ENABLED_CAPS_LEN];
    uint32_t enabled_api[ENABLED_API_LEN];
} fw_info_t;

#endif