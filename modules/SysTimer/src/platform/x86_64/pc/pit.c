/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <types.h>

#include "priv_timers.h"

// The 8254 PIT input frequency (Hz) -- a fixed reference present on every PC,
// including QEMU/KVM.
#define PIT_FREQ 1193182u

// Single owner of the PIT hardware. Other SysTimer code that needs a
// hardware-timed reference delay (e.g. the TSC calibration in tsc.c) calls
// pit_oneshot_wait_ns() rather than driving ports 0x42/0x43/0x61 itself, so the
// PIT is not bit-banged from several independent places.
//
// Implemented with channel 2 in mode 0 (interrupt-on-terminal-count) and polled
// via the OUT pin (port 0x61 bit 5). Channel 2 is used -- not channel 0 -- so
// this never disturbs the periodic IRQ0 system tick. The gate is enabled and the
// speaker output left off (port 0x61 bits 0/1). Pure polling, so it is safe with
// interrupts disabled. The 16-bit counter caps a single one-shot at ~54.9ms, so
// longer requests are serviced as a sequence of one-shots.
void pit_oneshot_wait_ns(uint64_t ns) {
    const uint64_t max_ns = 50ULL * 1000 * 1000;  // keep each count well within 16 bits
    while (ns > 0) {
        uint64_t chunk_ns = (ns > max_ns) ? max_ns : ns;
        uint32_t count = (uint32_t)((PIT_FREQ * chunk_ns) / 1000000000ULL);
        if (count == 0)
            count = 1;
        if (count > 0xFFFF)
            count = 0xFFFF;

        // Gate on (bit0), speaker off (bit1); program ch2, lobyte/hibyte, mode 0.
        outb(0x61, (uint8_t)((inb(0x61) & (uint8_t)~0x02) | 0x01));
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(count & 0xff));
        outb(0x42, (uint8_t)((count >> 8) & 0xff));

        // Poll OUT (bit5) for terminal count, with a safety bound so a missing
        // PIT cannot hang init forever.
        for (uint64_t guard = 0; !(inb(0x61) & 0x20); guard++)
            if (guard > 100000000ULL)
                return;

        ns -= chunk_ns;
    }
}

// The PIT is not (yet) wired up as a system/persistent timer -- it is used only
// as the calibration reference above. plat.c falls back here when there is no
// HPET; report "not a usable main timer" so the APIC timer path is taken.
int pit_init() {
    return -1;
}
