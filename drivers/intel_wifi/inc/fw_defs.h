// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef IWIFI_FW_DEFS_H
#define IWIFI_FW_DEFS_H

#include <stdint.h>
#include <stddef.h>

#define UCODE_MAGIC 0x0a4c5749

struct ucode_hdr {
    uint32_t zr0;
    uint32_t magic;
    uint8_t name[64];
    uint32_t version;
    uint32_t build;
    uint64_t rsv;
    uint8_t data[0];
};

struct ucode_tlv {
    uint32_t type;
    uint32_t length;
    uint8_t data[0];
};

enum ucode_tlv_type {
    UCODE_TLV_INVALID = 0,
    UCODE_TLV_INST = 1,
    UCODE_TLV_DATA = 2,
    UCODE_TLV_INIT = 3,
    UCODE_TLV_INIT_DATA = 4,
    UCODE_TLV_BOOT = 5,
    UCODE_TLV_PROBE_MAX_LEN = 6,
    UCODE_TLV_PAN = 7,
    UCODE_TLV_RUNT_EVTLOG_PTR = 8,
    UCODE_TLV_RUNT_EVTLOG_SIZE = 9,
    UCODE_TLV_RUNT_ERRLOG_PTR = 10,
    UCODE_TLV_INIT_EVTLOG_PTR = 11,
    UCODE_TLV_INIT_EVTLOG_SIZE = 12,
    UCODE_TLV_INIT_ERRLOG_PTR = 13,
    UCODE_TLV_ENHANCE_SENS_TBL = 14,
    UCODE_TLV_PHY_CALIBRATION_SIZE = 15,
    UCODE_TLV_WOWLAN_INST = 16,
    UCODE_TLV_WOWLAN_DATA = 17,
    UCODE_TLV_FLAGS = 18,
    UCODE_TLV_SEC_RT = 19,
    UCODE_TLV_SEC_INIT = 20,
    UCODE_TLV_SEC_WOWLAN = 21,
    UCODE_TLV_DEF_CALIB = 22,
    UCODE_TLV_PHY_SKU = 23,
    UCODE_TLV_SECURE_SEC_RT = 24,
    UCODE_TLV_SECURE_SEC_INIT = 25,
    UCODE_TLV_SECURE_SEC_WOWLAN = 26,
    UCODE_TLV_NUM_OF_CPU = 27,
    UCODE_TLV_CSCHEME = 28,
    UCODE_TLV_API_CHANGES_SET = 29,
    UCODE_TLV_ENABLED_CAPABILITIES = 30,
    UCODE_TLV_N_SCAN_CHANNELS = 31,
    UCODE_TLV_PAGING = 32,
    UCODE_TLV_SEC_RT_USNIFFER = 34,
    UCODE_TLV_SDIO_ADMA_ADDR = 35,
    UCODE_TLV_FW_VERSION = 36,
    UCODE_TLV_FW_DBG_DEST = 38,
    UCODE_TLV_FW_DBG_CONF = 39,
    UCODE_TLV_FW_DBG_TRIGGER = 40,
    UCODE_TLV_FW_GSCAN_CAPA = 50,
    UCODE_TLV_FW_MEM_SEG = 51,
};

struct tlv_def_calib {
    uint32_t type;
    uint32_t flow_trigger;
    uint32_t event_trigger;
};

#endif