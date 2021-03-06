
<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

Lots of work seems to be handled by the firmware.
```
- 0x000 : CSR_HW_IF_CONFIG_REG    :
    - PREPARE = 0x08000000
    - NIC_READY = 0x00400000
```
```
- 0x008 : CSR_INT:
    - Interrupt Status, same bits as CSR_INT_MASK
```
```
- 0x010 : FH_INT_STS:
    - FH interrupt status
```
```
- 0x00c : CSR_INT_MASK:
    - FW_ALIVE =    0x00000001
    - WAKEUP =      0x00000002
    - RX_CMD_RESP = 0x00000008
    - OVERHEAT =    0x00000020
    - HW_KILL =     0x00000040
    - FW_ERR =      0x02000000
    - TXQ_ADV =     0x04000000
    - FH_RX =       0x80000000
```
```
- 0x020 : CSR_RESET:
    - SW_RESET =    0x00000080
```
```
- 0x024 : CSR_GP_CNTRL:
    - HW_RF_KILL_SW = 0x08000000 
```
```
- 0x028 : 32-bit Hardware Revision Register, used to identify specific version:
    - 7000 Series Chips:
        - 31-16: Reserved
        - 15-4: Device type (see IWM_CSR_HW_REV_TYPE in if_iwmreg.h in freebsd)
        - 3-2: Revision step (A,B,C,D)
        - 1-0: Minor Revision number
        
    - 8000 Series Chips:
        - 31-16: Reserved
        - 15-4: Device type (see IWM_CSR_HW_REV_TYPE in if_iwmreg.h in freebsd)
        - 3-0: Revision step (A,B,C,D,...)
```
```
- 0x088 : CSR_MBOX_SET_REG:
    - OS_ALIVE = 0x20
```
```
- 0x0A8 : CSR_MAC_SHADOW_REG_CTRL
```
## Initialization Steps

1. Start by making the NIC prepare for use by setting the PREPARE and NIC_READY bits in CSR_HW_IF_CONFIG_REG, wait for NIC_READY to read as set.

2. Signal that the OS is ready by setting the OS_ALIVE bit in CSR_MBOX_SET_REG

3. Start allocating the various DMA buffers
        - A buffer for firmware transfers, 0x20000 byte region, 16-byte aligned

        - A buffer for the tx scheduler, 1024-byte aligned
        - A 'keep warm' page (4096 bytes) for internal use by the device, 4096-byte aligned
        - An rx ring - 256-byte aligned
        - tx rings - 256 byte aligned

4. Startup the hardware (iwm_start_hw)
    - Reset the device by setting the SW_RESET bit of CSR_RESET and waiting 10us
    - Setup and enable the associated HW_KILL interrupt.

5. Start the firmware (iwm_run_init_mvm_ucode)
    - Stop if RF kill
    - Set iwm_wait_phy_db_entry to be called on notification of both IWM_INIT_COMPLETE_NOTIF and IWM_CALIB_RES_NOTIF_PHY_DB
        - Parses and stores some information regarding the PAPD (?) and TXP (?)
    - Load ucode and wait for the fw to report being alive (WIP, iwm_mvm_load_ucode_wait_alive)
        - Read the firmware (iwm_read_firmware)
            - Load firmware into driver memory
            - Parse firmware contents (WIP)
        - Set iwm_alive_fn to be called on notification of MVM_ALIVE
        - Start the firmware (WIP, iwm_start_fw)
            - Check if NIC_READY bit is set, if not, redo step 1
            - Clear interrupt flags, disable interrupts
            - Ensure rfkill handshake is clear
                - Set RFKILL bit in CSR_UCODE_DRV_GP1_CLR
                - Set CMD_BLOCKED bit in CSR_UCODE_DRV_GP1_CLR
            - Init the NIC (iwm_nic_init)
                - Configure NIC (WIP, iwm_mvm_nic_config)
                    - Get the radio information (currently from the firmware, as we haven't read the nvm yet) and configure the NIC's radios appropriately
                - Setup RX queues (WIP, iwm_nic_rx_init)
                    - Stop RX dma (iwm_pcie_rx_stop)
                    - Reset pointers
                    - Set physical address of RX ring
                    - Set physical address of RX status
                    - Enable RX DMA
                    - Set the RB count
                - Setup TX queues (WIP, iwm_nic_tx_init)
                    - Deactivate TX scheduler (write 0 to peripheral register, SCD_TXFACT)
                    - Set physical address of 'keep warm' page
                    - Set physical addresses of TX rings
                    - Set queues to auto-activate (set bit 18 of peripheral register, SCD_GP_CTRL)
                - Enable MAC shadow registers by writing 0x800fffff to CSR_MAC_SHADOW_REG_CTRL

            - Disable all interrupts besides FH_TX
            - Ensure rfkill handshake is still clear
            - Load the firmware (WIP, iwm_pcie_load_given_ucode, iwm_pcie_load_given_ucode_8000)
                - For 7000 Series
                    - Load the cpu sections (iwm_pcie_load_cpu_sections)
                        - See 'Firmware Transfers' section for details.
                    - If dual CPU
                        - Write the CPU2 header address
                        - Load the cpu sections for cpu2 (WIP, iwm_pcie_load_cpu_sections)
                    - Re-enable interrupts
                    - Release the CPUs from reset (Set CSR_RESET to 0)
                - For 8000 Series
                    - 
        - Wait for MVM_ALIVE notification
        - Setup the tx scheduler (WIP, iwm_trans_pcie_fw_alive)
        - Configure firmware paging if necessary, based on the ucode information (WIP, iwm_save_fw_paging, iwm_send_paging_cmd)
    - Initialize nvm/eeprom (WIP, iwm_nvm_init)
        - Reads and parses EEPROM
    - Initialize Bluetooth config (WIP, iwm_send_bt_init_conf)
        - Sends Bluetooth initialization command
    - Get valid TX antennas (WIP, iwm_mvm_get_valid_tx_ant)
        - Part of the information obtained from the eeprom
    - Send valid TX antennas (WIP, iwm_send_tx_ant_cfg)
        - Sends a command with the previously obtained antenna configuration
    - Send physical config command to initialize ucode, and start callibrations (WIP, iwm_send_phy_cfg_cmd)
        - Sends PHY_CONFIGURATION command
    - Wait for init notification
        - Wait for callibration complete notification

6. Initialize the channel map (WIP, iwm_init_channel_map)
    - Parse nvm data to determine the supported channels and wifi bands supported by the device

7. Attach the radiotap and register to the wifi stack (WIP, iwm_radiotap_attach)


## Scan Steps

1. If firmware reports UMAC scan support:
    Setup a scan command with the desired scan channels and send to the device

2. Otherwise, if firmware reports LMAC scan support:
    Setup a scan command with the desired scan channels and send to the device


## Transmit Steps
Fill a tx command to send to the firmware, encrypting the frame if necessary

## Firmware Transfers
Firmware transfers are done via a separate DMA engine.
Sections are sent to it in chunks of 0x20000 bytes.

Target offsets over 0x40000 and under 0x57fff are treated as extended memory.

Target offset of 0xFFFFCCCC specifies the CPU1 and CPU2 separator section.
Target offset of 0xAAAABBBB specifies the paging separator section.

See iwm_pcie_load_firmware_chunk for more information on how to program the DMA engine.

## TX/TFD Scheduler
Each TX scheduler entry takes the following format:
```
struct tx_scheduler_entry {
    struct {
        uint16_t tx_cmd_byte_count : 12;
        uint16_t station_idx : 4;
    } tfd_queue[256 + 64];
}
```

There are as many tx_scheduler_entry structures as queues (MAX_QUEUES = 31).

Each TFD queue has a max size of 256 entries, freebsd driver adds 64 'BC_DUP' entries too, reason unknown right now.

## TX Ring
There are 256 TX rings, each ring has the following structure:
```
struct tx_ring_entry {
    uint32_t lo_addr;
    uint16_t hi_addr : 4;
    uint16_t tx_buf_len : 12;
} PACKED
```

```
lo_addr : lo component of the DMA buffer address
hi_addr : hi component of the DMA buffer address 
tx_buf_len : length of the DMA buffer
```

Additionally, each ring has a command buffer to submit commands to the firmware.

There are two command formats:
```
struct tx_cmd {
    struct cmd_hdr hdr;
    uint8_t data[320];
}
```
```
struct tx_cmd_wide {
    struct cmd_hdr_wide hdr;
    uint8_t data[316];
}
```

### Command Headers
```
struct cmd_hdr {
    uint8_t opcode;
    uint8_t flags;
    uint8_t index;
    uint8_t queue_id;
}
```

```
struct cmd_hdr_wide {
    uint8_t opcode;
    uint8_t group_id;
    uint8_t index;
    uint8_t queue_id;
    uint16_t length;
    uint8_t rsv;
    uint8_t ver;
}
```

opcodes:
```
enum {
    MVM_ALIVE = 0x1,
    REPLY_ERROR = 0x2,

    INIT_COMPLETE_NOTIF = 0x4,

    PHY_CONTEXT_CMD = 0x8,
    DBG_CFG = 0x9,

    SCAN_ITERATION_COMPLETE_UMAC = 0xb5,
    SCAN_CFG_CMD = 0xc,
    SCAN_REQ_UMAC = 0xd,
    SCAN_ABORT_UMAC = 0xe,
    SCAN_COMPLETE_UMAC = 0xf,

    ADD_STA_KEY = 0x17,
    ADD_STA = 0x18,
    REMOVE_STA = 0x19,

    TX_CMD = 0x1c,
    TXPATH_FLUSH = 0x1e,
    MGMT_MCAST_KEY = 0x1f,

    SCD_QUEUE_CFG = 0x1d,

    WEP_KEY = 0x20,

    MAC_CONTEXT_CMD = 0x28,
    TIME_EVENT_CMD = 0x29, /* both CMD and response */
    TIME_EVENT_NOTIFICATION = 0x2a,
    BINDING_CONTEXT_CMD = 0x2b,
    TIME_QUOTA_CMD = 0x2c,
    NON_QOS_TX_COUNTER_CMD = 0x2d,

    LQ_CMD = 0x4e,

    FW_PAGING_BLOCK_CMD = 0x4f,

    SCAN_OFFLOAD_REQUEST_CMD = 0x51,
    SCAN_OFFLOAD_ABORT_CMD = 0x52,
    HOT_SPOT_CMD = 0x53,
    SCAN_OFFLOAD_COMPLETE = 0x6d,
    SCAN_OFFLOAD_UPDATE_PROFILES_CMD = 0x6e,
    SCAN_OFFLOAD_CONFIG_CMD = 0x6f,
    MATCH_FOUND_NOTIFICATION = 0xd9,
    SCAN_ITERATION_COMPLETE = 0xe7,

    PHY_CONFIGURATION_CMD = 0x6a,
    CALIB_RES_NOTIF_PHY_DB = 0x6b,
    PHY_DB_CMD = 0x6c,

    POWER_TABLE_CMD = 0x77,
    PSM_UAPSD_AP_MISBEHAVING_NOTIFICATION = 0x78,

    REPLY_THERMAL_MNG_BACKOFF = 0x7e,

    SCAN_ABORT_CMD = 0x81,
    SCAN_START_NOTIFICATION = 0x82,
    SCAN_RESULTS_NOTIFICATION = 0x83,

    NVM_ACCESS_CMD = 0x88,

    SET_CALIB_DEFAULT_CMD = 0x8e,

    BEACON_NOTIFICATION = 0x90,
    BEACON_TEMPLATE_CMD = 0x91,
    TX_ANT_CONFIGURATION_CMD = 0x98,
    BT_CONFIG = 0x9b,
    STATISTICS_NOTIFICATION = 0x9d,
    REDUCE_TX_POWER_CMD = 0x9f,

    CARD_STATE_CMD = 0xa0,
    CARD_STATE_NOTIFICATION = 0xa1,

    MISSED_BEACONS_NOTIFICATION = 0xa2,

    MFUART_LOAD_NOTIFICATION = 0xb1,

    MAC_PM_POWER_TABLE = 0xa9,

    REPLY_RX_PHY_CMD = 0xc0,
    REPLY_RX_MPDU_CMD = 0xc1,
    BA_NOTIF = 0xc5,

    MCC_UPDATE_CMD = 0xc8,
    MCC_CHUB_UPDATE_CMD = 0xc9,

    BT_COEX_PRIO_TABLE = 0xcc,
    BT_COEX_PROT_ENV = 0xcd,
    BT_PROFILE_NOTIFICATION = 0xce,
    BT_COEX_CI = 0x5d,

    REPLY_SF_CFG_CMD = 0xd1,
    REPLY_BEACON_FILTERING_CMD = 0xd2,

    CMD_DTS_MEASUREMENT_TRIGGER = 0xdc,
    DTS_MEASUREMENT_NOTIFICATION = 0xdd,

    REPLY_DEBUG_CMD = 0xf0,
    DEBUG_LOG_MSG = 0xf7,

    MCAST_FILTER_CMD = 0xd0,

    D3_CONFIG_CMD = 0xd3,
    PROT_OFFLOAD_CONFIG_CMD = 0xd4,
    OFFLOADS_QUERY_CMD = 0xd5,
    REMOTE_WAKE_CONFIG_CMD = 0xd6,

    WOWLAN_PATTERNS = 0xe0,
    WOWLAN_CONFIGURATION = 0xe1,
    WOWLAN_TSC_RSC_PARAM = 0xe2,
    WOWLAN_TKIP_PARAM = 0xe3,
    WOWLAN_KEK_KCK_MATERIAL = 0xe4,
    WOWLAN_GET_STATUSES = 0xe5,
    WOWLAN_TX_POWER_PER_DB = 0xe6,

    NET_DETECT_CONFIG_CMD = 0x54,
    NET_DETECT_PROFILES_QUERY_CMD = 0x56,
    NET_DETECT_PROFILES_CMD = 0x57,
    NET_DETECT_HOTSPOTS_CMD = 0x58,
    NET_DETECT_HOTSPOTS_QUERY_CMD = 0x59,

    REPLY_MAX = 0xff,
}
```

## RX Ring (256-byte aligned)
There are up to 4096 RX rings (only power of two sizes supported), represented by an array of 32-bit addresses, each ring entry has a byte aligned 32-bit address pointing to an RX buffer. (RX_BUFFER_SIZE = 4096 or 8192 or 16384)

## RX Status Area (16-byte aligned)
This is a single region, that takes the following format:
```
struct rx_status {
    uint16_t closed_rb_num;
    uint16_t closed_frame_num;
    uint16_t finished_rb_num;
    uint16_t finished_frame_num;
    uint32_t rsv;
}
```
```
closed_rb_num [0:11] : the index of the reserve buffer that was last closed
closed_frame_num [0:11] : the index of the frame that was last closed
finished_rb_num [0:11] : the index of the current reserve buffer, to which the previous frame was written
finished_frame_num [0:11] : the index of the frame which was last transferred
```
## RF Hardware switch kill functionality
Read CSR_GP_CNTRL, bit HW_RF_KILL_SW to get the switch status, 
    0 = RF kill is on, turn off radio
    1 = RF kill is off, can turn on radio

## Notification Mechanism
Firmware related communications are all done via queues, the device responds by sending response commands via the rx ring. The freebsd driver queues these into a notification queue.
