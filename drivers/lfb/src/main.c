/**
 * Copyright (c) 2018 hgoel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "SysReg/registry.h"
#include "SysVirtualMemory/vmem.h"

#include "CoreDisplay/display.h"

static uint64_t fbuf_phys_addr;
static uint64_t pitch;
static uint64_t width;
static uint64_t height;

static int drv_getframebuffer(void *state, uintptr_t *addr) {
    state = NULL;
    *addr = (uintptr_t)vmem_phystovirt((intptr_t)fbuf_phys_addr, pitch * height, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    return 0;
}

static int drv_getstatus(void *state, display_status_t *ans) {
    state = NULL;
    *ans = display_status_connected;
    return 0;
}

static int drv_getdisplayinfo(void *state, display_res_info_t *res, int *entcnt) {

    state = NULL;
    *entcnt = 1;
    if(res != NULL) {
        res->w_res = width;
        res->h_res = height;
        res->stride = pitch;
        res->refresh_rate = 60;
    }
    return 0;
}

static display_desc_t display_config = {
    .display_name = "Linear Framebuffer",
    .connection = display_connection_unkn,
    .handlers = {
        .set_resolution = NULL,
        .set_brightness = NULL,
        .set_state = NULL,
        .get_framebuffer = drv_getframebuffer,
        .get_status = drv_getstatus,
        .get_displayinfo = drv_getdisplayinfo,
        .flush = NULL,
    },
    .features = 0,
    .state = NULL,
};

int module_init(void) {

    registry_readkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PHYS_ADDR", &fbuf_phys_addr);
    registry_readkey_uint("HW/BOOTINFO/FRAMEBUFFER", "PITCH", &pitch);
    registry_readkey_uint("HW/BOOTINFO/FRAMEBUFFER", "WIDTH", &width);
    registry_readkey_uint("HW/BOOTINFO/FRAMEBUFFER", "HEIGHT", &height);

    display_register(&display_config);

    uint32_t *addr = (uint32_t*)vmem_phystovirt((intptr_t)fbuf_phys_addr, pitch * height, vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    //memset(addr, 0xff, pitch * height);

    return 0;
}