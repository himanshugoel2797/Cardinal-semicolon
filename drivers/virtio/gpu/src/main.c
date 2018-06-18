/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "virtio.h"
#include "virtio_gpu.h"

static virtio_gpu_driver_state_t device;

int module_init(void *ecam) {
    device.common_state = virtio_initialize(ecam);

    uint32_t features = virtio_getfeatures(device.common_state, 0);
    __asm__("cli\n\thlt" :: "a"(features), "b"(0xdeadbeef));

    return 0;
}