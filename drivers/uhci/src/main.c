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
#include "SysTimer/timer.h"
#include "SysTaskMgr/task.h"
#include "pci/pci.h"
#include "CoreUsb/usb.h"

#include "uhci.h"

static uhci_ctrl_state_t *instances = NULL;
static int instance_count = 0;
static int instance_lock = 0;

static void write8(uhci_ctrl_state_t *state, uint16_t addr, uint8_t val)
{
    outb(state->iobar + addr, val);
}

static void write16(uhci_ctrl_state_t *state, uint16_t addr, uint16_t val)
{
    outw(state->iobar + addr, val);
}

static void write32(uhci_ctrl_state_t *state, uint16_t addr, uint32_t val)
{
    outl(state->iobar + addr, val);
}

static uint8_t read8(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inb(state->iobar + addr);
}

static uint16_t read16(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inw(state->iobar + addr);
}

static uint32_t read32(uhci_ctrl_state_t *state, uint16_t addr)
{
    return inl(state->iobar + addr);
}

// task_sleep does not deschedule (see notes/AUDIT.md) and timer_timestamp_ns is
// unreliable here (it uses floating point, which kernel modules build without,
// and depends on a counter timer that may be absent -- it can return (uint64_t)-1,
// collapsing a timestamp-based wait to zero), so use a plain bounded busy-spin for
// the one-time init delays. Not wall-clock precise -- only "at least roughly this
// long" -- which is all the USB reset/recovery delays need.
static void uhci_delay_ns(uint64_t ns)
{
    // ~a few cycles per iteration; one iteration per nanosecond is comfortably
    // generous on the emulated targets (over-waiting on init is harmless).
    for (volatile uint64_t i = 0; i < ns; i++)
        ;
}

static void uhci_reset(uhci_ctrl_state_t *state)
{
    write16(state, USBCMD_REG, USBCMD_GRESET);
    uhci_delay_ns(10 * 1000 * 1000);
    write16(state, USBCMD_REG, 0);  //Exit GRESET 10ms after starting

    //now perform an HCRESET (bounded wait for it to clear)
    write16(state, USBCMD_REG, USBCMD_HCRESET);
    for (volatile uint64_t i = 0; i < 50000000; i++)
        if (!(read16(state, USBCMD_REG) & USBCMD_HCRESET))
            break;
    if (read16(state, USBCMD_REG) & USBCMD_HCRESET)
        DEBUG_PRINT("[UHCI] HCRESET timeout\r\n");
    write32(state, FRBASEADDR_REG, state->framelist_pmem);
    write16(state, USBCMD_REG, 1);
}

static void uhci_enableport(uhci_ctrl_state_t *state, int idx)
{
    uint16_t v = read16(state, PORTSCn_REG(idx));
    
    //Reset port (>=10ms asserted), de-assert, then a recovery delay before enable.
    write16(state, PORTSCn_REG(idx), PORTSC_PORTRESET);
    uhci_delay_ns(15 * 1000 * 1000);
    write16(state, PORTSCn_REG(idx), 0);
    uhci_delay_ns(20 * 1000 * 1000);
    write16(state, PORTSCn_REG(idx), PORTSC_PORTEN | PORTSC_PORTENCHG);
    uhci_delay_ns(20 * 1000 * 1000);
}

// Synchronous control transfer on endpoint 0, used by CoreUsb for enumeration
// and by class drivers. Builds a SETUP/DATA/STATUS TD chain in the per-controller
// DMA scratch, links it under the persistent control QH, and polls to completion.
// Returns bytes transferred on the data stage (0 for a no-data control), or <0 on
// timeout/STALL/error. See notes/drivers/uhci-enumeration.md.
static int uhci_control_transfer(void *hc_state, int dev_addr, usb_speed_t speed,
                                 int max_packet, const usb_setup_packet_t *setup,
                                 void *data, int data_len)
{
    uhci_ctrl_state_t *st = (uhci_ctrl_state_t *)hc_state;
    int low_speed = (speed == usb_speed_low) ? 1 : 0;
    if (max_packet <= 0)
        max_packet = 8;
    if (data_len < 0)
        data_len = 0;
    if (data_len > UHCI_DATA_MAX)
        data_len = UHCI_DATA_MAX;

    local_spinlock_lock(&st->ctrl_lock);

    uint8_t *setup_buf = st->dma_virt + UHCI_SETUP_OFF;
    uint8_t *data_buf = st->dma_virt + UHCI_DATA_OFF;
    transfer_descriptor_t *tds = (transfer_descriptor_t *)(st->dma_virt + UHCI_TD_OFF);
    uhci_qh_t *qh = (uhci_qh_t *)(st->dma_virt + UHCI_QH_OFF);
    uint32_t setup_phys = (uint32_t)(st->dma_phys + UHCI_SETUP_OFF);
    uint32_t data_phys = (uint32_t)(st->dma_phys + UHCI_DATA_OFF);
    uint32_t td_phys_base = (uint32_t)(st->dma_phys + UHCI_TD_OFF);

    memcpy(setup_buf, setup, sizeof(usb_setup_packet_t));

    int is_read = (setup->bmRequestType & USB_REQ_DIR_IN) != 0;
    if (data_len > 0 && !is_read && data != NULL)
        memcpy(data_buf, data, data_len);

    int n = 0;
    // SETUP stage (always 8 bytes, DATA0).
    memset(&tds[n], 0, sizeof(transfer_descriptor_t));
    tds[n].status.status = 0x80;  // Active
    tds[n].status.err_count = 3;
    tds[n].status.ls = low_speed;
    tds[n].token.pid = UHCI_PID_SETUP;
    tds[n].token.device = dev_addr;
    tds[n].token.endpoint = 0;
    tds[n].token.data_toggle = 0;
    tds[n].token.maxlen = (8 - 1) & 0x7FF;
    tds[n].buffer_ptr = setup_phys;
    n++;

    // DATA stage (if any), max_packet chunks, toggle alternating from 1.
    int data_pid = is_read ? UHCI_PID_IN : UHCI_PID_OUT;
    int toggle = 1;
    int rem = data_len;
    uint32_t bptr = data_phys;
    while (rem > 0 && n < UHCI_TD_COUNT - 1) {
        int chunk = rem > max_packet ? max_packet : rem;
        memset(&tds[n], 0, sizeof(transfer_descriptor_t));
        tds[n].status.status = 0x80;
        tds[n].status.err_count = 3;
        tds[n].status.ls = low_speed;
        tds[n].token.pid = data_pid;
        tds[n].token.device = dev_addr;
        tds[n].token.endpoint = 0;
        tds[n].token.data_toggle = toggle;
        tds[n].token.maxlen = (chunk - 1) & 0x7FF;
        tds[n].buffer_ptr = bptr;
        n++;
        toggle ^= 1;
        rem -= chunk;
        bptr += chunk;
    }

    // STATUS stage: opposite direction, zero length, DATA1.
    int status_pid = is_read ? UHCI_PID_OUT : UHCI_PID_IN;
    int status_idx = n;
    memset(&tds[n], 0, sizeof(transfer_descriptor_t));
    tds[n].status.status = 0x80;
    tds[n].status.err_count = 3;
    tds[n].status.ls = low_speed;
    tds[n].token.pid = status_pid;
    tds[n].token.device = dev_addr;
    tds[n].token.endpoint = 0;
    tds[n].token.data_toggle = 1;
    tds[n].token.maxlen = 0x7FF;  // zero-length
    tds[n].buffer_ptr = 0;
    n++;

    // Link the chain depth-first; last TD terminates.
    for (int i = 0; i < n; i++) {
        if (i == n - 1)
            tds[i].link.lp = 1;  // Terminate
        else
            tds[i].link.lp = (td_phys_base + (i + 1) * sizeof(transfer_descriptor_t)) | (1 << 2);  // depth-first, TD
    }

    // Arm: QH element -> first TD (QH head terminates; we run a single QH).
    qh->hlp = 1;
    qh->elp = td_phys_base;

    // Poll until the STATUS TD retires, a fatal error latches, or we time out.
    // Bounded by iteration count (timer_timestamp_ns is unreliable here); the HC
    // updates TD status via DMA so success exits promptly.
    int result = -1;
    for (volatile uint64_t spin = 0; spin < 200000000ULL; spin++) {
        int fatal = 0;
        for (int i = 0; i < n; i++)
            if (tds[i].status.status & 0x76)  // STALL/databuf/babble/CRC-timeout/bitstuff (NAK excluded)
                fatal = 1;
        if (fatal)
            break;
        if (!(tds[status_idx].status.status & 0x80)) {  // status stage done
            result = 0;
            break;
        }
    }

    qh->elp = 1;  // detach the chain

    int transferred = -1;
    if (result == 0) {
        int total = 0;
        for (int i = 1; i < status_idx; i++) {
            uint32_t al = tds[i].status.act_len;
            total += (al == 0x7FF) ? 0 : (int)al + 1;
        }
        if (is_read && data != NULL && total > 0) {
            if (total > data_len)
                total = data_len;
            memcpy(data, data_buf, total);
        }
        transferred = total;
    }

    local_spinlock_unlock(&st->ctrl_lock);
    return transferred;
}

// Single-endpoint data transfer (interrupt or bulk) using the driver-tracked
// per-endpoint data toggle. Returns bytes transferred; 0 means "no data" for an
// IN poll that only saw NAKs within the spin budget; <0 on STALL/error.
static int uhci_data_transfer(uhci_ctrl_state_t *st, int dev_addr, int low_speed,
                              int endpoint, int dir_in, int max_packet,
                              void *data, int len, uint64_t spin_limit)
{
    if (max_packet <= 0)
        max_packet = 8;
    if (len < 0)
        len = 0;
    if (len > UHCI_DATA_MAX)
        len = UHCI_DATA_MAX;
    int ep = endpoint & 0xF;

    local_spinlock_lock(&st->ctrl_lock);

    uint8_t *data_buf = st->dma_virt + UHCI_DATA_OFF;
    transfer_descriptor_t *tds = (transfer_descriptor_t *)(st->dma_virt + UHCI_TD_OFF);
    uhci_qh_t *qh = (uhci_qh_t *)(st->dma_virt + UHCI_QH_OFF);
    uint32_t data_phys = (uint32_t)(st->dma_phys + UHCI_DATA_OFF);
    uint32_t td_phys_base = (uint32_t)(st->dma_phys + UHCI_TD_OFF);

    if (!dir_in && data != NULL && len > 0)
        memcpy(data_buf, data, len);

    int tidx = (dev_addr & 0x7F) * 16 + ep;
    int toggle = st->ep_toggle[tidx] & 1;

    int n = 0;
    int rem = len;
    uint32_t bptr = data_phys;
    int pid = dir_in ? UHCI_PID_IN : UHCI_PID_OUT;
    do {
        int chunk = rem > max_packet ? max_packet : rem;
        memset(&tds[n], 0, sizeof(transfer_descriptor_t));
        tds[n].status.status = 0x80;
        tds[n].status.err_count = 3;
        tds[n].status.ls = low_speed;
        tds[n].token.pid = pid;
        tds[n].token.device = dev_addr;
        tds[n].token.endpoint = ep;
        tds[n].token.data_toggle = toggle;
        tds[n].token.maxlen = (chunk > 0 ? (chunk - 1) : 0x7FF) & 0x7FF;
        tds[n].buffer_ptr = chunk > 0 ? bptr : 0;
        n++;
        toggle ^= 1;
        rem -= chunk;
        bptr += chunk;
    } while (rem > 0 && n < UHCI_TD_COUNT);

    for (int i = 0; i < n; i++)
        tds[i].link.lp = (i == n - 1) ? 1 : ((td_phys_base + (i + 1) * sizeof(transfer_descriptor_t)) | (1 << 2));

    qh->hlp = 1;
    qh->elp = td_phys_base;

    int result = -1;  // -1 timeout/NAK, -2 fatal, 0 success
    for (volatile uint64_t spin = 0; spin < spin_limit; spin++) {
        int fatal = 0, active = 0;
        for (int i = 0; i < n; i++) {
            uint32_t s = tds[i].status.status;
            if (s & 0x80)
                active = 1;
            if (s & 0x76)
                fatal = 1;
        }
        if (fatal) {
            result = -2;
            break;
        }
        if (!active) {
            result = 0;
            break;
        }
    }

    qh->elp = 1;

    int transferred;
    if (result == 0) {
        int total = 0;
        for (int i = 0; i < n; i++) {
            uint32_t al = tds[i].status.act_len;
            total += (al == 0x7FF) ? 0 : (int)al + 1;
        }
        if (dir_in && data != NULL && total > 0) {
            if (total > len)
                total = len;
            memcpy(data, data_buf, total);
        }
        st->ep_toggle[tidx] = (uint8_t)(toggle & 1);  // advanced once per TD
        transferred = total;
    } else if (result == -1 && dir_in) {
        transferred = 0;  // NAK within budget: no data available right now
    } else {
        transferred = -1;  // fatal, or OUT that didn't complete
    }

    local_spinlock_unlock(&st->ctrl_lock);
    return transferred;
}

static int uhci_interrupt_in(void *hc_state, int dev_addr, usb_speed_t speed, int endpoint,
                             int max_packet, int data_toggle, void *data, int len)
{
    (void)data_toggle;  // driver tracks the toggle internally
    uhci_ctrl_state_t *st = (uhci_ctrl_state_t *)hc_state;
    int low_speed = (speed == usb_speed_low) ? 1 : 0;
    return uhci_data_transfer(st, dev_addr, low_speed, endpoint, 1, max_packet, data, len, 5000000ULL);
}

static int uhci_bulk(void *hc_state, int dev_addr, int endpoint, int max_packet,
                     int data_toggle, void *data, int len, int dir_in)
{
    (void)data_toggle;
    uhci_ctrl_state_t *st = (uhci_ctrl_state_t *)hc_state;
    return uhci_data_transfer(st, dev_addr, 0, endpoint, dir_in, max_packet, data, len, 200000000ULL);
}

static void intr_handler(uhci_ctrl_state_t *inst){
    while (!inst->init_complete)
        ;
    while (true)
    {
        uint16_t sts = read16(inst, USBSTS_REG);

        for (int i = 0; i < PORT_COUNT; i++){
            uint16_t p_sts = read16(inst, PORTSCn_REG(i));

            if (p_sts & PORTSC_CONNECTCHG)
                write16(inst, PORTSCn_REG(i), PORTSC_CONNECTCHG);  //ack change

            bool connected = (p_sts & PORTSC_CURCONNECT) != 0;
            if (connected && !inst->port_enum[i]) {
                inst->port_enum[i] = true;
                DEBUG_PRINT("[UHCI] Device connected; resetting + enumerating\r\n");
                uhci_enableport(inst, i);
                uint16_t after = read16(inst, PORTSCn_REG(i));
                usb_speed_t spd = (after & PORTSC_LOWSPEED) ? usb_speed_low : usb_speed_full;
                usb_port_connected(inst->handle, i, spd);
            } else if (!connected && inst->port_enum[i]) {
                inst->port_enum[i] = false;
                DEBUG_PRINT("[UHCI] Device disconnected\r\n");
            }
        }

        if (sts & USBSTS_USBINT)
        {
            DEBUG_PRINT("[UHCI] USB Interrupt\r\n");
            write16(inst, USBSTS_REG, USBSTS_USBINT); //clear the interrupt
            continue;
        }
        task_yield(); //halt(); //swap for yield later
    }
}

int module_init(void *ecam_addr)
{
    pci_config_t *device = (pci_config_t *)vmem_phystovirt((intptr_t)ecam_addr, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //enable pci bus master
    device->command.busmaster = 1;

    // UHCI is driven by a cooperative polling task rather than interrupts: the
    // PIIX/ICH9 UHCI function has no MSI capability, and its legacy INTx# routes
    // through a PCI link whose IOAPIC GSI is only discoverable via ACPI _PRT --
    // which this kernel does not yet parse (interrupt_mapinterrupt maps a known
    // GSI, but we have no way to learn UHCI's). Transfer completion does not
    // actually need an interrupt here: the controller writes TD status to DMA
    // memory, which uhci_*_transfer polls directly. The clean interrupt path is
    // on xHCI (MSI); see notes/servers/CoreUsb-status.md. The poll task also
    // handles port connect/enumeration cooperatively (it task_yield()s).
    uhci_ctrl_state_t *instance = malloc(sizeof(uhci_ctrl_state_t));

    int cli_state = cli();
    local_spinlock_lock(&instance_lock);
    instance->id = instance_count++;
    instance->next = instances;
    instances = instance;
    local_spinlock_unlock(&instance_lock);
    sti(cli_state);

    uint64_t bar = (device->bar[4] & 0xFFFFFFF0); //I/O space BAR
    instance->iobar = bar;
    { char b[20]; DEBUG_PRINT("[UHCI] iobar="); DEBUG_PRINT(itoa((int)bar, b, 16)); DEBUG_PRINT("\r\n"); }

    //Frame list: 4x1024 entries
    //Each frame contains transfer descriptors
    //Queue Heads are for bulk transfers
    uintptr_t framelist_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data | physmem_alloc_flags_zero, KiB(4));
    if (framelist_phys == PHYSMEM_NO_ALLOC)
    {
        DEBUG_PRINT("[UHCI] Out of memory allocating frame list.\r\n");
        return -1;
    }
    instance->framelist_pmem = (uint32_t)framelist_phys;
    instance->framelist = (uhci_framelist_entry_t *)vmem_phystovirt((intptr_t)instance->framelist_pmem, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);
    instance->init_complete = false;
    instance->ctrl_lock = 0;
    for (int i = 0; i < PORT_COUNT; i++)
        instance->port_enum[i] = false;

    //Allocate the control-transfer DMA scratch (QH + TD pool + buffers).
    uintptr_t dma_phys = pagealloc_alloc(0, 0, physmem_alloc_flags_32bit | physmem_alloc_flags_data | physmem_alloc_flags_zero, KiB(4));
    if (dma_phys == PHYSMEM_NO_ALLOC)
    {
        DEBUG_PRINT("[UHCI] Out of memory allocating DMA scratch.\r\n");
        return -1;
    }
    instance->dma_phys = dma_phys;
    instance->dma_virt = (uint8_t *)vmem_phystovirt((intptr_t)dma_phys, KiB(4), vmem_flags_uncached | vmem_flags_kernel | vmem_flags_rw);

    //Point every frame at the (idle) control QH so submitted transfers run; the
    //QH's element pointer terminates until a transfer is armed.
    uhci_qh_t *qh = (uhci_qh_t *)(instance->dma_virt + UHCI_QH_OFF);
    qh->hlp = 1;  // Terminate
    qh->elp = 1;  // Terminate (idle)
    for (int i = 0; i < FRAME_COUNT; i++)
        instance->framelist[i].flp = (uint32_t)dma_phys | (1 << 1);  // is_qh, valid

    usb_hci_desc_t *desc = malloc(sizeof(usb_hci_desc_t));
    memset(desc, 0, sizeof(usb_hci_desc_t));
    itoa(instance->id, desc->name, 10);
    desc->state = instance;
    desc->device_type = usb_device_type_uhci;
    desc->handlers.control = uhci_control_transfer;
    desc->handlers.interrupt_in = uhci_interrupt_in;
    desc->handlers.bulk = uhci_bulk;
    desc->lock = 0;
    usb_register_hostcontroller(desc, &instance->handle);

    //Reset HCI
    uhci_reset(instance);

    //Start polling process (also handles connect detection + enumeration)
    cs_id int_task = 0;
    create_task_kernel("uhci_int_poll", task_permissions_kernel, &int_task);
    start_task_kernel(int_task, (void (*)(void *))intr_handler, instance);
    instance->intr_task = int_task;

    instance->init_complete = true;

    DEBUG_PRINT("[UHCI] Init complete\r\n");

    return 0;
}