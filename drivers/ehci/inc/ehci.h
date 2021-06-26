// Copyright (c) 2021 himanshu
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_EHCI_DRIV_H
#define CARDINALSEMI_EHCI_DRIV_H

#include <stdint.h>

typedef struct {
    uint8_t caplength;
    uint8_t rsv0;
    uint16_t hciversion;
    struct {
        uint32_t n_ports : 4;
        uint32_t ppc : 1;
        uint32_t rsv0 : 2;
        uint32_t routing_rules : 1;
        uint32_t n_pcc : 4;
        uint32_t n_cc : 4;
        uint32_t p_indicator : 1;
        uint32_t rsv1 : 3;
        uint32_t dbg_pc : 4;
        uint32_t rsv2 : 8;
    } hcsparams;
    struct {
        uint32_t _64b : 1;
        uint32_t prog_framelist : 1;
        uint32_t async_park : 1;
        uint32_t rsv0 : 1;
        uint32_t iso_thresh : 4;
        uint32_t eecp : 8;
        uint32_t rsv1 : 16;
    } hccparams;
    uint64_t hcsp_portroute;
} PACKED ehci_cap_registers_t;

typedef struct {
    struct {
        uint32_t rs : 1;
        uint32_t hcreset : 1;
        uint32_t fl_size : 2;
        uint32_t periodic_sched : 1;
        uint32_t async_sched : 1;
        uint32_t ioa_doorbell : 1;
        uint32_t lhcreset : 1;
        uint32_t async_mode_count : 2;
        uint32_t rsv0 : 1;
        uint32_t async_mode_en : 1;
        uint32_t rsv1 : 4;
        uint32_t itc : 8;
        uint32_t rsv2 : 8;
    } usbcmd;
    struct {
        uint32_t usbint : 1;
        uint32_t usberrint : 1;
        uint32_t port_chg_det : 1;
        uint32_t frame_list_rollover : 1;
        uint32_t host_sys_err : 1;
        uint32_t ioa : 1;
        uint32_t rsv0 : 6;
        uint32_t hchalted : 1;
        uint32_t reclamation : 1;
        uint32_t period_sched : 1;
        uint32_t async_sched : 1;
        uint32_t rsv1 : 16;
    } usbsts;
    struct {
        uint32_t usbint : 1;
        uint32_t usberrint : 1;
        uint32_t port_chg_int : 1;
        uint32_t frame_list_rollover : 1;
        uint32_t host_sys_err : 1;
        uint32_t ioa : 1;
        uint32_t rsv : 26;
    } usbintr;
    uint32_t  frindex;
    uint32_t ctrldssegment;
    uint32_t periodiclistbase;
    uint32_t asynclistaddr;
    uint32_t rsv[10];
    uint32_t configflag;
    struct {
        uint32_t connect_sts : 1;
        uint32_t connect_sts_chg : 1;
        uint32_t port_en : 1;
        uint32_t port_en_chg : 1;
        uint32_t oc : 1;
        uint32_t oc_chg : 1;
        uint32_t resume : 1;
        uint32_t suspend : 1;
        uint32_t port_reset : 1;
        uint32_t rsv0 : 1;
        uint32_t line_sts : 2;
        uint32_t port_power : 1;
        uint32_t port_owner : 1;
        uint32_t port_indicator : 2;
        uint32_t port_test_ctl : 4;
        uint32_t wkcnnt_e : 1;
        uint32_t wkdscnnt_e : 1;
        uint32_t wkoc_e : 1;
        uint32_t rsv1 : 9;
    } portsc[0];
} PACKED ehci_op_registers_t;

typedef struct ehci_ctrl_state
{
    uint32_t phys_membar;
    volatile uint8_t *membar;
    volatile ehci_cap_registers_t *capreg;
    volatile ehci_op_registers_t *opreg;
    uint32_t framelist_pmem;
    uint32_t *framelist;
    volatile bool init_complete;
    void *handle;
    cs_id intr_task;
    int id;

    struct ehci_ctrl_state *next;
} ehci_ctrl_state_t;

#endif