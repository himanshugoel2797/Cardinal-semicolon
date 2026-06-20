/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <types.h>

#include "SysInterrupts/interrupts.h"
#include "SysVirtualMemory/vmem.h"
#include "SysReg/registry.h"

#include "elf.h"
#include "tls_util.h"

#define ICW4_8086 0x01    /* 8086/88 (MCS-80/85) mode */
#define ICW1_ICW4 0x01    /* ICW4 (not) needed */
#define ICW1_INIT 0x10 /* Initialization - required! */

#define APIC_ID (0x20)
#define APIC_TPR (0x80)
#define APIC_PPR (0xA0)
#define APIC_EOI (0xB0)
#define APIC_LDR (0xD0)
#define APIC_DFR (0xE0)
#define APIC_SVR (0xF0)
#define APIC_ISR (0x100)
#define APIC_ICR_xAPIC_LO (0x300)
#define APIC_ICR_xAPIC_HI (0x310)
#define APIC_ICR_x2APIC (0x300)

#define APIC_TIMER (0x320)
#define APIC_ICoR (0x380)
#define APIC_CCoR (0x390)
#define APIC_DCR (0x3E0)

#define MSI_ADDR (0xFEEFF00C)
#define MSI_VEC (lvl, active_low, vector) (((lvl & 1) << 15) | ((~active_low & 1) << 14) | 0x100 /*lowest priority*/ | (vector & 0xff))

typedef struct {
    uint32_t *base_addr;
    uint32_t id;
    bool x2apic_mode;
} tls_apic_t;

static TLS tls_apic_t *apic = NULL;

uint64_t apic_read(uint32_t off) {
    if(apic->x2apic_mode) {
        uint64_t val = rdmsr(0x800 + off / 16);
        return val;
    }
    return apic->base_addr[off / sizeof(uint32_t)];
}

void apic_write(uint32_t off, uint64_t val) {
    if(apic->x2apic_mode) {

        if(off == APIC_EOI) {
            wrmsr(0x800 + off / 16, val);
            return;
        }
        uint64_t tmp_Val = rdmsr(0x800 + off / 16);
        wrmsr(0x800 + off / 16, (tmp_Val & ~0xffffffff) | val);
    } else
        apic->base_addr[off / sizeof(uint32_t)] = (uint32_t)val;
}

int pic_fini() {
    //Disable the PIC
    outb(0x20, ICW1_INIT+ICW1_ICW4); // starts the initialization sequence (in cascade mode)
    outb(0xA0, ICW1_INIT+ICW1_ICW4);
    outb(0x21, 32);           // ICW2: Master PIC vector offset
    outb(0xA1, 40);           // ICW2: Slave PIC vector offset
    outb(0x21, 4);                 // ICW3: tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    outb(0xA1, 2);                 // ICW3: tell Slave PIC its cascade identity (0000 0010)

    outb(0x21, ICW4_8086);
    outb(0xA1, ICW4_8086);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF); //disable all interrupts from the PIC

    return 0;
}

int apic_init() {

    if (apic == NULL)
        apic = (TLS tls_apic_t *)intr_tls_alloc(sizeof(tls_apic_t));


    uint64_t apic_base_reg = rdmsr(IA32_APIC_BASE);
    apic->base_addr = (uint32_t*)vmem_phystovirt(apic_base_reg & ~0xfff, KiB(8), vmem_flags_uncached);
    apic_base_reg |= (1 << 11); //Enable the apic
    {
        bool x2apic_sup = false;
        if(registry_readkey_bool("HW/PROC", "X2APIC", &x2apic_sup) != CS_OK)
            return -1;

        apic->x2apic_mode = x2apic_sup;

        if(x2apic_sup) {
            apic_base_reg |= (1 << 10);   //Enable x2apic mode

        } else {

        }
    }
    if(!apic->x2apic_mode) apic_write(APIC_DFR, 0xf0000000);    //Setup cluster destination mode
    wrmsr(IA32_APIC_BASE, apic_base_reg);

    //Read the id after enabling the apic so x2apic handling works correctly
    apic->id = apic_read(APIC_ID);
    if(!apic->x2apic_mode)apic->id = apic->id >> 24;

    apic_write(APIC_TPR, 0);
    apic_write(APIC_SVR, (1 << 8) | 0xFF);

    //TODO: Implement IPI function

    return 0;
}

int interrupt_get_cpu_idx(void) {
    return apic->id;
}

void interrupt_sendeoi(int irq) {
    int byte_off = (irq / 32) * 16;
    int bit_off = irq % 32;

    if(apic_read(APIC_ISR + byte_off) & (1 << bit_off)) {
        apic_write(APIC_EOI, 0);
    }
}

void interrupt_sendipi(int cpu, int vector, ipi_delivery_mode_t delivery_mode) {
    uint64_t ipi_msg = 0;
    ipi_msg |= (vector & 0xff);
    ipi_msg |= (delivery_mode & 0x7) << 8;
    ipi_msg |= (1 << 14);

    if(apic->x2apic_mode) {
        ipi_msg |= ((uint64_t)cpu << 32);

        apic_write(APIC_ICR_x2APIC, ipi_msg);
    } else {
        ipi_msg |= ((uint64_t)cpu << 56);

        apic_write(APIC_ICR_xAPIC_HI, (uint32_t)(ipi_msg >> 32));
        apic_write(APIC_ICR_xAPIC_LO, (uint32_t)ipi_msg);
    }
}

static int intrpt_num = 0;

//configure timer for tsc deadline
// Measure the local APIC timer's input frequency against PIT channel 2 (a
// fixed 1.193182 MHz source present on every PC, including QEMU). Used when
// CPUID/MSR don't report the bus frequency (e.g. under QEMU/KVM, where the
// CPUID-based path in SysReg/cpuid.c yields APIC_FREQ == 0). Returns the bus
// frequency in Hz (pre-divider), matching the meaning of HW/PROC/APIC_FREQ.
#define PIT_FREQ 1193182u
static uint64_t apic_timer_calibrate(void) {
    const uint32_t calib_ms = 10;
    uint16_t pit_count = (uint16_t)((uint64_t)PIT_FREQ * calib_ms / 1000); //~11932

    // PIT channel 2: gate on (0x61 bit0), speaker off (bit1); mode 0 one-shot.
    outb(0x61, (inb(0x61) & (uint8_t)~0x02) | 0x01);
    outb(0x43, 0xB0); //ch2, lobyte/hibyte, mode 0, binary

    // Arm the APIC timer: divide by 2, masked one-shot, max initial count.
    apic_write(APIC_DCR, 0x0);
    apic_write(APIC_TIMER, (1u << 16)); //masked
    apic_write(APIC_ICoR, 0xFFFFFFFFu);

    // Load the PIT count (starts it), then poll OUT (0x61 bit5) for terminal count.
    outb(0x42, (uint8_t)(pit_count & 0xff));
    outb(0x42, (uint8_t)((pit_count >> 8) & 0xff));
    while (!(inb(0x61) & 0x20))
        ;

    uint32_t remaining = (uint32_t)apic_read(APIC_CCoR);
    apic_write(APIC_ICoR, 0); //stop
    uint32_t elapsed = 0xFFFFFFFFu - remaining;

    // elapsed ticks happened in calib_ms at divide-by-2; undo both to get the
    // pre-divider bus frequency in Hz.
    return (uint64_t)elapsed * (1000 / calib_ms) * 2;
}

int local_apic_timer_init(bool tsc_mode, void (*handler)(int), bool ap) {

    if(!ap) {
        intrpt_num = 50;   //Allocate a low priority interrupt
        interrupt_allocate(1, interrupt_flags_exclusive, &intrpt_num);
        interrupt_register_handler(intrpt_num, handler);
    }

    uint64_t v = apic_read(APIC_TIMER);
    v = v & ~0x007100ff;
    v |= (intrpt_num & 0xff);

    uint64_t initial_count = 0;
    if(tsc_mode) {
        v |= (2 << 17); //Set TSC-Deadline mode
    } else {
        v |= (1 << 17); //Set Periodic mode

        uint64_t apic_freq = 0;
        if(registry_readkey_uint("HW/PROC", "APIC_FREQ", &apic_freq) != CS_OK)
            apic_freq = 0;

        //CPUID/MSR did not report a usable bus frequency (e.g. QEMU/KVM): fall
        //back to measuring it against the PIT. Cache it so APs and repeat calls
        //don't re-run the 10ms calibration (the rate is uniform across cores).
        if(apic_freq < 20000) {
            static uint64_t calibrated_apic_freq = 0;
            if(calibrated_apic_freq == 0)
                calibrated_apic_freq = apic_timer_calibrate();
            apic_freq = calibrated_apic_freq;
        }

        apic_write(APIC_DCR, 0x0); //divide by 2 (matches the calibration)
        // The local-APIC timer is the OS scheduler tick. Under the
        // interpreter-as-scheduler model it is NOT a preemption quantum -- the
        // only thing it drives is waking (sleep)/wait-with-timeout sleepers (see
        // SysLisp lisp_timer_tick), so millisecond granularity is ample. The old
        // 20kHz (50us) rate -- a leftover from the preemptive scheduler -- pinned
        // the resident loop near 100% CPU under KVM: each tick is a HLT/wake round
        // trip, and a 50us period is shorter than that round trip's cost, so the
        // HLT never actually sleeps. 250Hz (4ms) keeps sleeps responsive while
        // letting the HLT idle between ticks. (TICKLESS -- arming the timer only
        // for the next pending sleeper deadline -- would idle deeper still; left
        // as a follow-up.) initial_count = apic_freq/N gives an N Hz tick here.
        initial_count = apic_freq / 250; // 250Hz scheduler tick (4ms granularity)
    }

    //Program the LVT (mode + vector + unmask) BEFORE loading the initial count.
    //Writing APIC_ICoR is what arms the timer, and in periodic mode the count
    //must be loaded while the LVT already holds periodic mode -- otherwise (the
    //previous order: ICoR then LVT) the count was loaded while the LVT still held
    //the calibration routine's masked one-shot config, so it counted to zero with
    //no interrupt and stopped, and periodic ticks never started. TCG's APIC model
    //tolerated this; KVM's faithful in-kernel APIC did not, hanging the boot for
    //want of preemption ticks (no scheduler tick -> the BSP never leaves the boot
    //path for idle/servicescript). See notes/AUDIT.md.
    apic_write(APIC_TIMER, v);
    if(!tsc_mode)
        apic_write(APIC_ICoR, initial_count);

    return intrpt_num;
}



//task switches don't require callibration
//sleep operations do require callibration - given frequency timers