<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

Lots of work seems to be handled by the firmware.

0x000 : CSR_HW_IF_CONFIG_REG
    - PREPARE = 0x08000000
    - NIC_READY = 0x00400000

0x028 : Hardware Revision Register, used to identify specific version

0x088 : CSR_MBOX_SET_REG

1. Start by making the NIC prepare for use by setting the PREPARE and NIC_READY bits in CSR_HW_IF_CONFIG_REG, wait for NIC_READY to read as set.

2. Signal that the OS is ready by setting the OS_ALIVE bit in CSR_MBOX_SET_REG

3. Start allocating the various DMA buffers
        - A buffer for firmware transfers, 0x20000 byte region, 16-byte aligned

        - A buffer for the tx scheduler, 1024-byte aligned
        - A 'keep warm' page (4096 bytes) for internal use by the device, 4096-byte aligned
        - An rx ring - 256-byte aligned
        - tx rings - 256 byte aligned

