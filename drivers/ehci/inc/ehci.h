// Copyright (c) 2021 himanshu
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_EHCI_DRIV_H
#define CARDINALSEMI_EHCI_DRIV_H

#include <stdint.h>

typedef struct ehci_ctrl_state
{
    uint32_t membar;
    uint32_t framelist_pmem;
    uint32_t *framelist;
    volatile bool init_complete;
    void *handle;
    cs_id intr_task;
    int id;

    struct ehci_ctrl_state *next;
} ehci_ctrl_state_t;

#endif