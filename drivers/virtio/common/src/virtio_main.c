/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <types.h>
#include <stdlib.h>
#include <string.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"
#include "pci/pci.h"
#include "virtio.h"

PRIVATE virtio_state_t* virtio_initialize(void *ecam_addr){
    pci_config_t *device = (pci_config_t*)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    virtio_state_t *n_state = malloc(sizeof(virtio_state_t));
    n_state->device = device;

    //TODO: install msi interrupt servicing
    //traverse capabilities
    if(device->capabilitiesPtr != 0) {
        uint8_t ptr = device->capabilitiesPtr;
        uint8_t *pci_base = (uint8_t*)device;

        do {
            pci_cap_header_t *capEntry = (pci_cap_header_t*)(pci_base + ptr);

            if(capEntry->capID == pci_cap_msi) {
                DEBUG_PRINT("MSI\r\n");
            } else if(capEntry->capID == pci_cap_msix) {
                DEBUG_PRINT("MSIX\r\n");
            } else if(capEntry->capID == pci_cap_vendor) {
                DEBUG_PRINT("Vendor\r\n");

                intptr_t bar_addr = 0;
                
                virtio_pci_cap_t *vendorCap = (virtio_pci_cap_t*)capEntry;
                if(device->bar[vendorCap->bar] & 4)
                    bar_addr = ((intptr_t)device->bar[vendorCap->bar + 1] << 32) + device->bar[vendorCap->bar] & 0xFFFF0000;
                else
                    bar_addr = device->bar[vendorCap->bar] & 0xFFFF0000;

                bar_addr = vmem_phystovirt(bar_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

                switch(vendorCap->cfg_type) {
                    case virtio_pci_cap_cfg_common:
                    {
                        n_state->common_cfg = (virtio_pci_common_cfg_t*)(bar_addr + vendorCap->offset);
                        DEBUG_PRINT("\tCOMMON\r\n");
                    }
                    break;
                    case virtio_pci_cap_cfg_notify:
                        DEBUG_PRINT("\tNOTIFY\r\n");
                    break;
                    case virtio_pci_cap_cfg_isr:{
                        n_state->isr_cfg = (virtio_pci_isr_cfg_t*)(bar_addr + vendorCap->offset);                        
                        DEBUG_PRINT("\tISR\r\n");
                    }
                    break;
                    case virtio_pci_cap_cfg_device:
                        DEBUG_PRINT("\tDEVICE\r\n");
                    break;
                    case virtio_pci_cap_cfg_pci:
                        DEBUG_PRINT("\tPCI\r\n");
                    break;
                }
            }
                ptr = capEntry->nextPtr;
        } while(ptr != 0);
    }

    //Awaken device

    //Set ack bit
    //Set driver bit
    n_state->common_cfg->device_status |= (1 << ACKNOWLEDGE);
    n_state->common_cfg->device_status |= (1 << DRIVER);



    return n_state;
}

//Read device features, write supported features
PRIVATE uint32_t virtio_getfeatures(virtio_state_t *state, int idx) {
    state->common_cfg->device_feature_select = idx;
    return state->common_cfg->device_feature;
}

PRIVATE void virtio_setfeatures(virtio_state_t *state, int idx, uint32_t val) {
    state->common_cfg->driver_feature_select = idx;
    state->common_cfg->driver_feature = val;
}

PRIVATE bool virtio_features_ok(virtio_state_t *state) {
    //set FEATURES_OK
    //re-read FEATURES_OK bit to confirm it was accepted
    state->common_cfg->device_status |= (1 << FEATURES_OK);
    return state->common_cfg->device_status & (1 << FEATURES_OK);
}

PRIVATE void virtio_driver_ok(virtio_state_t *state) {
    state->common_cfg->device_status |= (1 << DRIVER_OK);
}

PRIVATE void* virtio_setupqueue(virtio_state_t *state, int idx, int entcnt) {

    state->common_cfg->queue_select = (uint16_t)idx;
    state->common_cfg->queue_size = entcnt;

    size_t ts = 16 * entcnt + 6 + 2 * entcnt + 6 + 8 * entcnt;
    uintptr_t virtqueue_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, ts);
    intptr_t virtqueue_virt = vmem_phystovirt((intptr_t)virtqueue_phys, ts, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    memset((void*)virtqueue_virt, 0, ts);

    return (void*)virtqueue_virt;
}