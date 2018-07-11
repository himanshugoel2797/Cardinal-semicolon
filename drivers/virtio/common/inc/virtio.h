// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_VIRTIO_COMMON_MAIN_H
#define CARDINAL_VIRTIO_COMMON_MAIN_H

#include <types.h>
#include <stddef.h>

#include "pci/pci.h"

typedef struct {
    uint8_t capID;
    uint8_t nextPtr;
    uint8_t capLen;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t padding[3];
    uint32_t offset;
    uint32_t length;
} virtio_pci_cap_t;

typedef enum {
    virtio_pci_cap_cfg_common = 1,
    virtio_pci_cap_cfg_notify = 2,
    virtio_pci_cap_cfg_isr = 3,
    virtio_pci_cap_cfg_device = 4,
    virtio_pci_cap_cfg_pci = 5,
} virtio_pci_cap_cfg_type_t;

typedef struct {
    uint32_t device_feature_select;     /* read-write */
    uint32_t device_feature; /* read-only for driver */
    uint32_t driver_feature_select;     /* read-write */
    uint32_t driver_feature; /* read-write */
    uint16_t msix_config; /* read-write */
    uint16_t num_queues; /* read-only for driver */

#define ACKNOWLEDGE (1)
#define DRIVER (2)
#define FEATURES_OK (8)
#define DRIVER_OK (4)
    uint8_t device_status; /* read-write */
    uint8_t config_generation; /* read-only for driver */

    uint16_t queue_select; /* read-write */
    uint16_t queue_size; /* read-write, power of 2, or 0. */
    uint16_t queue_msix_vector; /* read-write */
    uint16_t queue_enable; /* read-write */
    uint16_t queue_notify_off; /* read-only for driver */
    uint64_t queue_desc; /* read-write */
    uint64_t queue_avail; /* read-write */
    uint64_t queue_used; /* read-write */
} virtio_pci_common_cfg_t;

typedef struct {
    virtio_pci_cap_t cap;
    uint32_t notify_multiplier;
} virtio_pci_notif_cfg_t;

/* Arbitrary descriptor layouts. */
#define VIRTIO_F_ANY_LAYOUT (1 << 27)
/* Support for indirect descriptors */
#define VIRTIO_F_INDIRECT_DESC  (1 << 28)
/* Support for avail_event and used_event fields */
#define VIRTIO_F_EVENT_IDX  (1 << 29)

/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT   1
/* This marks a buffer as device write-only (otherwise device read-only). */
#define VIRTQ_DESC_F_WRITE     2
/* This means the buffer contains a list of buffer descriptors. */
#define VIRTQ_DESC_F_INDIRECT   4

typedef struct {
    uint64_t addr;  /* Address (guest-physical). */
    uint32_t len;   /* Length. */
    uint16_t flags; /* The flags as indicated above. */
    uint16_t next;  /* Next field if flags & NEXT */
} virtq_desc_t;

typedef struct {
    uint16_t flags;
    volatile uint16_t idx;
    uint16_t ring[0];
} virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct PACKED {
    uint16_t flags;
    volatile uint16_t idx;
    virtq_used_elem_t ring[0];
} virtq_used_t;

typedef struct {
    void *virt;
    intptr_t phys;
    size_t len;
} virtio_phys_virt_addrpair_t;

typedef struct virtio_virtq_cmd_state {
    virtio_phys_virt_addrpair_t cmd;
    virtio_phys_virt_addrpair_t resp;
    bool waiting;
    bool finished;
    int q;
    int idx;
    void (*handler)(struct virtio_virtq_cmd_state *);
} virtio_virtq_cmd_state_t;

typedef struct {
    virtio_pci_common_cfg_t *common_cfg;
    volatile uint32_t *isr_cfg;
    virtio_pci_notif_cfg_t *notif_cfg;
    void *dev_cfg;

    int *avail_idx;
    int *used_idx;
    virtio_virtq_cmd_state_t **cmds;

    intptr_t notif_bar;
    pci_config_t *device;
} virtio_state_t;

PRIVATE virtio_state_t* virtio_initialize(void *ecam_addr, void (*int_handler)(int), virtio_virtq_cmd_state_t **cmds, int *avail_idx, int *used_idx);

PRIVATE uint32_t virtio_getfeatures(virtio_state_t *state, int idx);

PRIVATE void virtio_setfeatures(virtio_state_t *state, int idx, uint32_t val);

PRIVATE bool virtio_features_ok(virtio_state_t *state);

PRIVATE void virtio_driver_ok(virtio_state_t *state);

PRIVATE void* virtio_setupqueue(virtio_state_t *state, int idx, int entcnt);

PRIVATE void virtio_notify(virtio_state_t *state, int idx);

PRIVATE void virtio_addresponse(virtio_state_t *state, int idx, void *buf, int len, void (*resp_handler)(virtio_virtq_cmd_state_t*));

PRIVATE void virtio_postcmd_noresp(virtio_state_t *state, int idx, void *cmd, int len, void (*resp_handler)(virtio_virtq_cmd_state_t*));

PRIVATE void virtio_postcmd(virtio_state_t *state, int idx, void *cmd, int len, void *resp, int response_len, void (*resp_handler)(virtio_virtq_cmd_state_t*));

PRIVATE void virtio_accept_used(virtio_state_t *state, int idx);

#endif