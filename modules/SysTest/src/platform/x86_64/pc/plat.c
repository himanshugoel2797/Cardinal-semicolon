// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <types.h>

#include "priv_test.h"

// QEMU's isa-debug-exit device (`-device isa-debug-exit,iobase=0xf4,iosize=0x04`):
// an OUT to 0xf4 makes QEMU exit with status (value << 1) | 1. We encode pass as
// 0x10 (-> exit 33) and fail as 0x11 (-> exit 35) so a CI harness can branch on
// the exit code; it should also grep the serial log for the pass/fail sentinel.
// If the device is absent the OUT is a no-op, so we fall back to halting.
#define ISA_DEBUG_EXIT_PORT 0xf4
#define QEMU_EXIT_PASS 0x10
#define QEMU_EXIT_FAIL 0x11

void NORETURN test_platform_exit(int code) {
    outb(ISA_DEBUG_EXIT_PORT, code == 0 ? QEMU_EXIT_PASS : QEMU_EXIT_FAIL);

    // Fallback if isa-debug-exit is not wired up: stop the machine.
    __asm__ volatile("cli");
    while (1)
        __asm__ volatile("hlt");
}

// Reboot the machine. Used by the death-test path: after a death is recorded on
// the harness control channel, the guest resets and the harness advances the
// death cursor on the next boot. Tries the PCI/ICH9 reset-control register
// (0xCF9, reliable on QEMU q35 under both KVM and TCG), then the 8042 keyboard
// controller, then a forced triple fault via a null IDT. Never returns.
void NORETURN system_reset(void) {
    __asm__ volatile("cli");

    // 0xCF9: set RST_CPU|SYS_RST. Write 0x02 (assert) then 0x06 (full reset).
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);

    // 8042: pulse the CPU reset line. Wait for the input buffer to clear first.
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }
    outb(0x64, 0xFE);

    // Last resort: load a zero-length IDT and raise an interrupt -> triple fault.
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } null_idt = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int3");

    while (1)
        __asm__ volatile("hlt");
}
