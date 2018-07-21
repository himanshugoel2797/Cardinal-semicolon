/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "initrd.h"

#include "if_iwmreg.h"
#include "devices.h"
#include "device_helpers.h"
#include "firmware.h"
#include "fw_defs.h"

static void fw_addsection(fw_info_t *fw_inf, fw_section_type_t type, void *data, size_t data_len) {
    uint32_t sec_ent = fw_inf->section_entries[type].section_ent;
    data_len -= sizeof(uint32_t);
    fw_inf->section_entries[type].sections[sec_ent].data = malloc(data_len);
    memcpy(fw_inf->section_entries[type].sections[sec_ent].data, (uint8_t*)data + sizeof(uint32_t), data_len);
    fw_inf->section_entries[type].sections[sec_ent].len = data_len;
    fw_inf->section_entries[type].sections[sec_ent].offset = *(uint32_t*)data;
    fw_inf->section_entries[type].section_ent++;
}

static int fw_parse(void *data, size_t data_len, fw_info_t *fw_inf) {

    memset(fw_inf, 0, sizeof(fw_info_t));

    struct ucode_hdr *hdr = (struct ucode_hdr*)data;
    uint8_t *data_u8 = (uint8_t*)hdr->data;

    if(hdr->magic != UCODE_MAGIC)
        return -1;

    data_len -= sizeof(struct ucode_hdr);

    while(data_len > 0) {
        data_len -= sizeof(struct ucode_tlv);
        struct ucode_tlv *tlv = (struct ucode_tlv*)data_u8;

        size_t tlv_len = tlv->length;
        if(tlv_len % sizeof(uint32_t))
            tlv_len += sizeof(uint32_t) - (tlv_len % sizeof(uint32_t));

        data_len -= tlv_len;

        data_u8 += sizeof(struct ucode_tlv);
        data_u8 += tlv_len;


        switch(tlv->type) {
            case UCODE_TLV_PROBE_MAX_LEN:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("PROBE_MAX_LEN\r\n");
#endif
                    fw_inf->probe_max_len = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_PAGING:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("PAGING\r\n");
#endif
                    fw_inf->paging_sz = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_PHY_SKU:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("PHY SKU\r\n");
#endif
                    fw_inf->phy_sku = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_NUM_OF_CPU:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("NUM OF CPU\r\n");
#endif
                    fw_inf->num_of_cpu = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_N_SCAN_CHANNELS:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("N SCAN CHANNELS\r\n");
#endif
                    fw_inf->n_scan_channels = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_FW_VERSION:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("FW VERSION\r\n");
#endif
                    fw_inf->fw_version[0] = ((uint32_t*)tlv->data)[0];
                    fw_inf->fw_version[1] = ((uint32_t*)tlv->data)[1];
                    fw_inf->fw_version[2] = ((uint32_t*)tlv->data)[2];
                }
                break;
            case UCODE_TLV_PAN:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("PAN\r\n");
#endif
                    fw_inf->flags |= 1;
                }
                break;
            case UCODE_TLV_FLAGS:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("FLAGS\r\n");
#endif
                    fw_inf->flags = *(uint32_t*)tlv->data;
                }
                break;
            case UCODE_TLV_SEC_RT:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("SEC RT\r\n");
#endif
                    fw_addsection(fw_inf, fw_section_regular, tlv->data, tlv_len);
                }
                break;
            case UCODE_TLV_SEC_INIT:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("SEC INIT\r\n");
#endif
                    fw_addsection(fw_inf, fw_section_init, tlv->data, tlv_len);
                }
                break;
            case UCODE_TLV_SEC_WOWLAN:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("SEC WOWLAN\r\n");
#endif
                    fw_addsection(fw_inf, fw_section_wowlan, tlv->data, tlv_len);
                }
                break;
            case UCODE_TLV_SEC_RT_USNIFFER:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("SEC RT USNIFFER\r\n");
#endif
                    fw_addsection(fw_inf, fw_section_usniffer, tlv->data, tlv_len);
                }
                break;
            case UCODE_TLV_ENABLED_CAPABILITIES:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("ENABLED CAPABILITIES\r\n");
#endif
                    uint32_t idx = ((uint32_t*)tlv->data)[0];
                    uint32_t flags = ((uint32_t*)tlv->data)[1];

                    if(idx < ENABLED_CAPS_LEN)
                        fw_inf->enabled_caps[idx] |= flags;
                }
                break;
            case UCODE_TLV_API_CHANGES_SET:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("API CHANGES SET\r\n");
#endif
                    uint32_t idx = ((uint32_t*)tlv->data)[0];
                    uint32_t flags = ((uint32_t*)tlv->data)[1];

                    if(idx < ENABLED_API_LEN)
                        fw_inf->enabled_api[idx] |= flags;
                }
                break;
            case UCODE_TLV_DEF_CALIB:
                {
#ifdef DEBUG_MSG
                    DEBUG_PRINT("DEF CALIB\r\n");
#endif
                    struct tlv_def_calib *calib_data = (struct tlv_def_calib*)tlv->data;
                    fw_inf->section_entries[calib_data->type].flow_trigger = calib_data->flow_trigger;
                    fw_inf->section_entries[calib_data->type].event_trigger = calib_data->event_trigger;
                }
                break;
            default:
#ifdef DEBUG_MSG
                DEBUG_PRINT("Unknown: %d\r\n", tlv->type);
#endif
            break;
        }
    }

    return 0;
}

int iwifi_fw_init(iwifi_dev_state_t *dev) {
    
    if(iwifi_check_rfkill(dev))
        return -1;

    //Get the firmware file from the initrd and parse
    void *fw_file = NULL;
    size_t fw_len = 0;
    if(!Initrd_GetFile(dev->device->fw_file, &fw_file, &fw_len))
        return -2;

    if(fw_parse(fw_file, fw_len, &dev->fw_info) != 0)
        return -3;

    //Start the firmware
    iwifi_disable_interrupts(dev);
    iwifi_clear_rfkillhandshake(dev);

    //Setup radio information
    uint8_t r_cfg_type, r_cfg_step, r_cfg_dash;
    uint32_t r_v = 0;
    uint32_t phy_config = dev->fw_info.phy_sku;

    r_cfg_type = (phy_config & IWM_FW_PHY_CFG_RADIO_TYPE) >> IWM_FW_PHY_CFG_RADIO_TYPE_POS;
    r_cfg_step = (phy_config & IWM_FW_PHY_CFG_RADIO_STEP) >> IWM_FW_PHY_CFG_RADIO_STEP_POS;
    r_cfg_dash = (phy_config & IWM_FW_PHY_CFG_RADIO_DASH) >> IWM_FW_PHY_CFG_RADIO_DASH_POS;

    r_v |= IWM_CSR_HW_REV_STEP(dev->hw_rev) << IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP;
    r_v |= IWM_CSR_HW_REV_DASH(dev->hw_rev) << IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH;
	r_v |= r_cfg_type << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
	r_v |= r_cfg_step << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
	r_v |= r_cfg_dash << IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;

    iwifi_write32(dev, IWM_CSR_HW_IF_CONFIG_REG, r_v);
    //NOTE: Potential issue here, if_iwm.c,1394

    //Setup RX queues
    iwifi_rx_dma_state(dev, false);

    //Reset RX pointers
    iwifi_write32(dev, IWM_FH_MEM_RCSR_CHNL0_RBDCB_WPTR, 0);
    iwifi_write32(dev, IWM_FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ, 0);
    iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RDPTR, 0);
    iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);

    //Configure RX queue addresses
	iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RBDCB_BASE_REG, dev->rx_mem.paddr >> 8);
    iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_STTS_WPTR_REG, dev->rx_status_mem.paddr >> 4);

    //Re-enable RX dma
    iwifi_rx_dma_state(dev, true);

    //Setup TX queues

    //Configure MAC shadowing
    iwifi_write32(dev, IWM_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);

    //Disable all interrupts besides FH_TX

    iwifi_clear_rfkillhandshake(dev);

    //Load the firmware

    return 0;
}