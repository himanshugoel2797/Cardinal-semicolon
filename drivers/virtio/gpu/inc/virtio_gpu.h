// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_VIRTIO_GPU_MAIN_H
#define CARDINAL_VIRTIO_GPU_MAIN_H

#include <stdint.h>
#include <stdbool.h>

#include "virtio.h"

#define VIRTIO_GPU_F_VIRGL (1 << 0)      //Virgl 3d support

#define VIRTIO_GPU_VIRTQUEUE_COUNT 2

typedef struct {
    uint32_t events_read;   /* read-only */
    uint32_t events_clear;  /* read-write */
    uint32_t num_scanouts;  /* read-only Number of simultaneous displays*/
    uint32_t rsvd;
} virtio_gpu_config_t;

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)

typedef struct {
    virtio_state_t *common_state;
    bool virgl_mode;
} virtio_gpu_driver_state_t;

#endif