// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_DEVICE_HELPERS_H
#define CARDINALSEMI_DEVICE_HELPERS_H

#include <stdint.h>
#include <stdbool.h>

#include "devices.h"

int iwifi_fw_init(iwifi_dev_state_t *dev);
void iwifi_prepare(iwifi_dev_state_t *state);
void iwifi_hw_start(iwifi_dev_state_t *state);
bool iwifi_check_rfkill(iwifi_dev_state_t *state);
void iwifi_disable_interrupts(iwifi_dev_state_t *state);
void iwifi_clear_rfkillhandshake(iwifi_dev_state_t *state);
void iwifi_rx_dma_state(iwifi_dev_state_t *state, bool enable);
void iwifi_tx_sched_dma_state(iwifi_dev_state_t *state, bool enable);
void iwifi_set_pwr(iwifi_dev_state_t *dev);

void iwifi_lock(iwifi_dev_state_t *state);
void iwifi_unlock(iwifi_dev_state_t *state);

#endif