// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_SYSGDB_H
#define CARDINALSEMI_SYSGDB_H

// Drop into the GDB stub and wait for a debugger (raises a breakpoint). Useful
// from a boot script to debug early boot.
void gdb_stub_wait(void);

// Replace the GDB serial channel (default COM2). A USB-serial driver calls this
// to route GDB over a USB-serial adapter. Both callbacks are blocking byte I/O.
void gdb_set_channel(int (*getc_fn)(void), void (*putc_fn)(int));

#endif
