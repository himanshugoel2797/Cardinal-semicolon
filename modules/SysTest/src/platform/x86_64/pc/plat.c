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
