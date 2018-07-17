/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <types.h>
#include <stdlib.h>
#include <string.h>

#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "SysInterrupts/interrupts.h"
#include "pci/pci.h"

#include "iwl-csr.h"

#include "devices.h"
#include "pwm.h"

static void intr_handler(int intr_num) {
    intr_num = 0;
}

int module_init(void *ecam_addr) {

    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    //identify the device
    iwifi_dev_state_t *dev_state = malloc(sizeof(iwifi_dev_state_t));
    if(iwifi_getdevice(device->deviceID, &dev_state->device) != 0) {
        DEBUG_PRINT("iwifi driver loaded for unsupported device!");
        return 0;
    }

    //interrupt setup
    int int_cnt = 0;
    int msi_val = pci_getmsiinfo(device, &int_cnt);

    if(msi_val < 0)
        DEBUG_PRINT("NO MSI\r\n");

    int int_val = 0;
    interrupt_allocate(1, interrupt_flags_none, &int_val);
    interrupt_registerhandler(int_val, intr_handler);

    uintptr_t msi_addr = (uintptr_t)msi_register_addr(0);
    uint32_t msi_msg = msi_register_data(int_val);
    pci_setmsiinfo(device, msi_val, &msi_addr, &msi_msg, 1);

    //figure out which bar to use
    uint64_t bar = 0;
    for(int i = 0; i < 6; i++) {
        if((device->bar[i] & 0x6) == 0x4) //Is 64-bit
            bar = (device->bar[i] & 0xFFFFFFF0) + ((uint64_t)device->bar[i + 1] << 32);
        else if((device->bar[i] & 0x6) == 0x0) //Is 32-bit
            bar = (device->bar[i] & 0xFFFFFFF0);
        if(bar) break;
    }
    dev_state->bar_phys = (uintptr_t)bar;
    dev_state->bar = (uint8_t*)vmem_phystovirt((intptr_t)bar, KiB(4), vmem_flags_rw | vmem_flags_uncached | vmem_flags_kernel);

    //Read basic device info
    dev_state->hw_rev = iwifi_read32(dev_state, IWM_CSR_HW_REV);

    //Setup the device
    iwifi_notify_ready(dev_state);

    //Allocate and configure memory for firmware transfer, kw, tx scheduler, tx, rx
    size_t fw_sz = IWM_FH_MEM_TB_MAX_LENGTH; //16-byte aligned
    size_t tx_sched_rings_sz = IWM_MVM_MAX_QUEUES * sizeof(struct iwm_agn_scd_bc_tbl); //1024-byte aligned
    size_t kw_page_sz = 4096;  //4096 byte aligned
    size_t rx_rings_sz = RX_RING_COUNT * sizeof(uint32_t) + sizeof(struct iwm_rb_status) + RBUF_SZ; //256-byte aligned
    size_t tx_rings_sz = TX_RING_COUNT * (sizeof(struct iwm_tfd) + sizeof(struct iwm_device_cmd)); //256-byte aligned

#define ROUNDUP(x, y) (((x - 1) | (y - 1)) + 1)

    size_t net_sz = ROUNDUP(fw_sz, 1024);
    net_sz = ROUNDUP((net_sz + tx_sched_rings_sz), 256);
    net_sz = ROUNDUP((net_sz + rx_rings_sz), 256);
    net_sz = ROUNDUP((net_sz + tx_rings_sz), 4096);
    net_sz += kw_page_sz;
    
    uintptr_t buf_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, net_sz);


    //Test the LEDs, as a visual debugging response

    //Load and setup the firmware

    //Find all supported channels and frequency bands

    DEBUG_PRINT("Wifi ready!\r\n");
    //Register device to network stack

    //Perform a scan to test the implementation, listing all detected wifi networks

    return 0;
}