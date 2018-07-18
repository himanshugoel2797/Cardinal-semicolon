
<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

Lots of work seems to be handled by the firmware.
```
- 0x000 : CSR_HW_IF_CONFIG_REG	:
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
        - Set iwm_alive_fn to be called on notification of IWM_MVM_ALIVE
        - Start the firmware (WIP, iwm_start_fw)
            - Check if NIC_READY bit is set, if not, redo step 1
            - Clear interrupt flags, disable interrupts
            - Ensure rfkill handshake is clear
                - Set RFKILL bit in CSR_UCODE_DRV_GP1_CLR
                - Set CMD_BLOCKED bit in CSR_UCODE_DRV_GP1_CLR
            - Init the NIC (iwm_nic_init)
                - Configure NIC (WIP, iwm_mvm_nic_config)
                    - Get the radio information (presumably from the firmware) and configure the NIC's radios appropriately
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
        - Wait for IWM_MVM_ALIVE notification
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

= Command Headers =
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
