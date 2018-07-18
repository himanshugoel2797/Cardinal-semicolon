<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

Lots of work seems to be handled by the firmware.

0x000 : CSR_HW_IF_CONFIG_REG
    - PREPARE = 0x08000000
    - NIC_READY = 0x00400000

0x008 : CSR_INT
    - Interrupt Status, same bits as CSR_INT_MASK

0x00c : CSR_INT_MASK
    - FW_ALIVE =    0x00000001
    - WAKEUP =      0x00000002
    - RX_CMD_RESP = 0x00000008
    - OVERHEAT =    0x00000020
    - HW_KILL =     0x00000040
    - FW_ERR =      0x02000000
    - TXQ_ADV =     0x04000000
    - FH_RX =       0x80000000

0x020 : CSR_RESET
    - SW_RESET =    0x00000080

0x024 : CSR_GP_CNTRL
    - HW_RF_KILL_SW = 0x08000000 

0x028 : 32-bit Hardware Revision Register, used to identify specific version
    7000 Series Chips:
        31-16: Reserved
        15-4: Device type (see IWM_CSR_HW_REV_TYPE in if_iwmreg.h in freebsd)
        3-2: Revision step (A,B,C,D)
        1-0: Minor Revision number

    8000 Series Chips:
        31-16: Reserved
        15-4: Device type (see IWM_CSR_HW_REV_TYPE in if_iwmreg.h in freebsd)
        3-0: Revision step (A,B,C,D,...)

0x088 : CSR_MBOX_SET_REG
    - OS_ALIVE = 0x20

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

5. Start the firmware (WIP, iwm_run_init_mvm_ucode)
    - Disable if RF kill
    - Wait for init notification (WIP, iwm_init_notification_wait)
    - Load ucode and wait for the fw to report being alive (WIP, iwm_mvm_load_ucode_wait_alive)
    - Initialize nvm (WIP, iwm_nvm_init)
    - Initialize Bluetooth config (WIP, iwm_send_bt_init_conf)
    - Get valid TX antennas (WIP, iwm_mvm_get_valid_tx_ant)
    - Send valid TX antennas (WIP, iwm_send_tx_ant_cfg)
    - Send physical config command to initialize ucode, and start callibrations (WIP, iwm_send_phy_cfg_cmd)
    - Wait for init notification (WIP, iwm_wait_notification)

6. Initialize the channel map (WIP, iwm_init_channel_map)
7. Attach the radiotap (WIP, iwm_radiotap_attach)




= TX/TFD Scheduler =
Each TX scheduler entry takes the following format:

struct tx_scheduler_entry {
    struct {
        uint16_t tx_cmd_byte_count : 12;
        uint16_t station_idx : 4;
    } tfd_queue[256 + 64];
}

There are as many tx_scheduler_entry structures as queues (MAX_QUEUES = 31).

Each TFD queue has a max size of 256 entries, freebsd driver adds 64 'BC_DUP' entries too, reason unknown right now.

= TX Ring =
There are 256 TX rings, each ring has the following structure:

struct tx_ring_entry {
    uint32_t lo_addr;
    uint16_t hi_addr : 4;
    uint16_t tx_buf_len : 12;
} PACKED

lo_addr : lo component of the DMA buffer address
hi_addr : hi component of the DMA buffer address 
tx_buf_len : length of the DMA buffer


Additionally, each ring has a command buffer to submit commands to the firmware.

There are two command formats:

struct tx_cmd {
    struct cmd_hdr hdr;
    uint8_t data[320];
}

struct tx_cmd_wide {
    struct cmd_hdr_wide hdr;
    uint8_t data[316];
}

= Command Headers =
struct cmd_hdr {
    uint8_t opcode;
    uint8_t flags;
    uint8_t index;
    uint8_t queue_id;
}

struct cmd_hdr_wide {
    uint8_t opcode;
    uint8_t group_id;
    uint8_t index;
    uint8_t queue_id;
    uint16_t length;
    uint8_t rsv;
    uint8_t ver;
}

= RX Ring (256-byte aligned) =
There are 256 RX rings, represented by an array of 32-bit addresses, each ring entry has a byte aligned 32-bit address pointing to an RX buffer. (RX_BUFFER_SIZE = 4096)

= RX Status Area (16-byte aligned) =
This is a single region, that takes the following format:

struct rx_status {
    uint16_t closed_rb_num;
    uint16_t closed_frame_num;
    uint16_t finished_rb_num;
    uint16_t finished_frame_num;
    uint32_t rsv;
}

closed_rb_num [0:11] : the index of the reserve buffer that was last closed
closed_frame_num [0:11] : the index of the frame that was last closed
finished_rb_num [0:11] : the index of the current reserve buffer, to which the previous frame was written
finished_frame_num [0:11] : the index of the frame which was last transferred

= RF Hardware switch kill functionality =
Read CSR_GP_CNTRL, bit HW_RF_KILL_SW to get the switch status, 
    0 = RF kill is on, turn off radio
    1 = RF kill is off, can turn on radio