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
#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"

#include "if_iwmreg.h"
#include "constants.h"
#include "devices.h"
#include "device_helpers.h"
#include "firmware.h"
#include "fw_defs.h"

#include <types.h>

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
                DEBUG_PRINT("Unknown\r\n");
#endif
            break;
        }
    }

    return 0;
}

void iwifi_push_dma(iwifi_dev_state_t *dev, uintptr_t paddr, uint32_t dest, uint32_t len){
    
    dev->fh_tx_int = 0;
    
    iwifi_lock(dev);

    iwifi_write32(dev, IWM_FH_TCSR_CHNL_TX_CONFIG_REG(IWM_FH_SRVC_CHNL), IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);

	iwifi_write32(dev, IWM_FH_SRVC_CHNL_SRAM_ADDR_REG(IWM_FH_SRVC_CHNL), dest);

	iwifi_write32(dev, IWM_FH_TFDIB_CTRL0_REG(IWM_FH_SRVC_CHNL), (uint32_t)paddr & IWM_FH_MEM_TFDIB_DRAM_ADDR_LSB_MSK);

	iwifi_write32(dev, IWM_FH_TFDIB_CTRL1_REG(IWM_FH_SRVC_CHNL), len);

	iwifi_write32(dev, IWM_FH_TCSR_CHNL_TX_BUF_STS_REG(IWM_FH_SRVC_CHNL),
	    1 << IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM |
	    1 << IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX |
	    IWM_FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);

	iwifi_write32(dev, IWM_FH_TCSR_CHNL_TX_CONFIG_REG(IWM_FH_SRVC_CHNL),
	    IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE    |
	    IWM_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
	    IWM_FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);

    iwifi_unlock(dev);

    //TODO: An FH_TX interrupt should be raised here
    while(!dev->fh_tx_int){
        if(iwifi_read32(dev, IWM_CSR_INT) & IWM_CSR_INT_BIT_FH_TX)
            DEBUG_PRINT("FH_TX should be ready\r\n");
        
        char tmp[10];
        DEBUG_PRINT("IWM_CSR_INT:");
        DEBUG_PRINT(itoa(iwifi_read32(dev, IWM_CSR_INT), tmp, 16));

        DEBUG_PRINT(" IWM_CSR_INT_MASK:");
        DEBUG_PRINT(itoa(iwifi_read32(dev, IWM_CSR_INT_MASK), tmp, 16));

        DEBUG_PRINT(" IWM_CSR_FH_INT_STATUS:");
        DEBUG_PRINT(itoa(iwifi_read32(dev, IWM_CSR_FH_INT_STATUS), tmp, 16));

        DEBUG_PRINT(" FH_TX Waiting\r\n");
    }
    DEBUG_PRINT("FW_TX SENT\r\n");
}

void iwifi_fw_dma(iwifi_dev_state_t *dev, fw_section_t *sect) {
    uint32_t chunk_sz = MIN(IWM_FH_MEM_TB_MAX_LENGTH, sect->len);

    //Allocate space for the chunk
    uintptr_t buf_p = pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data, IWM_FH_MEM_TB_MAX_LENGTH);
    uint8_t* buf_u8 = (uint8_t*)vmem_phystovirt((intptr_t)buf_p, IWM_FH_MEM_TB_MAX_LENGTH, vmem_flags_uncached | vmem_flags_rw | vmem_flags_kernel);

    //Submit the chunk to the FH dma
    for(uint32_t off = 0; off < sect->len; off += chunk_sz) {
        uint32_t cur_chunk_sz = MIN(chunk_sz, sect->len - off);
        uint32_t d_addr = sect->offset + off;

        iwifi_lock(dev);
        if(d_addr >= IWM_FW_MEM_EXTENDED_START && d_addr <= IWM_FW_MEM_EXTENDED_END)
            iwifi_periph_setbits32(dev, IWM_LMPM_CHICK, IWM_LMPM_CHICK_EXTENDED_ADDR_SPACE);
        iwifi_unlock(dev);

        memcpy(buf_u8, sect->data + off, cur_chunk_sz);
        iwifi_push_dma(dev, buf_p, d_addr, cur_chunk_sz);

        iwifi_lock(dev);
        if(d_addr >= IWM_FW_MEM_EXTENDED_START && d_addr <= IWM_FW_MEM_EXTENDED_END)
            iwifi_periph_clrbits32(dev, IWM_LMPM_CHICK, IWM_LMPM_CHICK_EXTENDED_ADDR_SPACE);
        iwifi_unlock(dev);
    }
}

void iwifi_load_fw(iwifi_dev_state_t *dev, int cpu, fw_section_type_t type, uint32_t *section_cursor) {
    int last_idx = 0;

    if(cpu == 1){
        (*section_cursor)++;
    }

    for(int i = *section_cursor; i < MAX_SECTION_COUNT; i++) {
        last_idx = i;

        if((dev->fw_info.section_entries[type].sections[i].offset == IWM_CPU1_CPU2_SEPARATOR_SECTION) ||
           (dev->fw_info.section_entries[type].sections[i].offset == IWM_PAGING_SEPARATOR_SECTION) ||
           (dev->fw_info.section_entries[type].sections[i].data == NULL))
                break;

        iwifi_fw_dma(dev, &dev->fw_info.section_entries[type].sections[i]);
    }

    *section_cursor = last_idx;
}

void iwifi_setup_fw(iwifi_dev_state_t *dev, fw_section_type_t type) {
    
    uint32_t section_cursor = 0;
    iwifi_load_fw(dev, 0, type, &section_cursor);
    if(dev->fw_info.num_of_cpu == 2) {
        //Load the CPU2 firmware
        iwifi_lock(dev);
        iwifi_periph_write32(dev, IWM_LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR, IWM_LMPM_SECURE_CPU2_HDR_MEM_SPACE);
        iwifi_unlock(dev);

        iwifi_load_fw(dev, 0, type, &section_cursor);
    }
}

int iwifi_prepare_card_hw(iwifi_dev_state_t *dev) {
    //iwm_set_hw_ready
        //SET(IF_CONFIG_REG, BIT_NIC_READY)
        //POLL(IF_CONFIG_REG, BIT_NIC_READY)
        //SET(MBOX_SET_REG, REG_OS_ALIVE)
    iwifi_setbits32(dev, IWM_CSR_HW_IF_CONFIG_REG, IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);

    while(true){
        if(iwifi_read32(dev, IWM_CSR_HW_IF_CONFIG_REG) & IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY == IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY)
            return 0;

        DEBUG_PRINT("WAITING on iwifi_prepare_card_hw\n");
    }
    return 1;
}

int iwifi_fw_init(iwifi_dev_state_t *dev) {
    
    //TODO: We don't check the rfkill switch the first time we init
    //if(iwifi_check_rfkill(dev))
    //    return -1;

    //Get the firmware file from the initrd and parse
    void *fw_file = NULL;
    size_t fw_len = 0;
    if(!Initrd_GetFile(dev->device->fw_file, &fw_file, &fw_len))
        return -2;

    if(fw_parse(fw_file, fw_len, &dev->fw_info) != 0)
        return -3;

    //iwm_start_fw
    iwifi_prepare_card_hw(dev);

    //Start the firmware
    iwifi_disable_interrupts(dev);
    iwifi_clear_rfkillhandshake(dev);

    iwifi_write(dev, IWM_CSR_INT, 0xffffffff);

    iwifi_hw_start(dev);

    //7000 only
    iwifi_set_pwr(dev);

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

    //7000 only
    iwifi_lock(dev);
    iwifi_periph_setbits_mask32(dev, IWM_APMG_PS_CTRL_REG, IWM_APMG_PS_CTRL_EARLY_PWR_OFF_RESET_DIS, ~IWM_APMG_PS_CTRL_EARLY_PWR_OFF_RESET_DIS);

    //Setup RX queues
    {
        iwifi_rx_dma_state(dev, false);

        //Reset RX pointers
        iwifi_write32(dev, IWM_FH_MEM_RCSR_CHNL0_RBDCB_WPTR, 0);
        iwifi_write32(dev, IWM_FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ, 0);
        iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RDPTR, 0);
        iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RBDCB_WPTR_REG, 0);

        //Configure RX queue addresses
	    iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_RBDCB_BASE_REG, dev->rx_mem.paddr >> 8);
        iwifi_write32(dev, IWM_FH_RSCSR_CHNL0_STTS_WPTR_REG, dev->rx_status_mem.paddr >> 4);

        iwifi_rx_dma_state(dev, true);
    }

    //Setup TX queues
    {
        iwifi_tx_sched_dma_state(dev, false);

        //Setup keep warm page
        iwifi_write32(dev, IWM_FH_KW_MEM_ADDR_REG, (uint32_t)dev->kw_mem.paddr >> 4);

        for(int i = 0; i < TX_RING_COUNT; i++) {
            if(i < ACTIVE_TX_RING_COUNT) {
                uintptr_t addr = dev->tx_mem.paddr + i * TX_RING_COUNT * sizeof(struct iwm_tfd);
                iwifi_write32(dev, IWM_FH_MEM_CBBC_QUEUE(i), addr >> 8);
            }else{
                iwifi_write32(dev, IWM_FH_MEM_CBBC_QUEUE(i), 0);
            }
        }

        iwifi_tx_sched_dma_state(dev, true);
    }
    iwifi_unlock(dev);

    //Configure MAC shadowing
    iwifi_write32(dev, IWM_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);

    //Enable FH_TX interrupt
    iwifi_write32(dev, IWM_CSR_INT_MASK, IWM_CSR_INT_BIT_FH_TX);
    dev->int_mask = IWM_CSR_INT_BIT_FH_TX;
    
    iwifi_clear_rfkillhandshake(dev);
    
    //Load the firmware
    iwifi_setup_fw(dev, fw_section_init);


    return 0;
}