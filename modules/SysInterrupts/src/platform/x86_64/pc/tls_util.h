/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#ifndef SYSINTERRUPTS_TLS_UTIL_H
#define SYSINTERRUPTS_TLS_UTIL_H

#include <types.h>
#include "elf.h"

/**
 * Allocate a per-core TLS slot of `sz` bytes and return the core-local pointer
 * to it.  This is a thin wrapper around the two-step idiom shared by gdt.c,
 * idt.c, and apic.c:
 *
 *   int (*mp_tls_alloc)(int) = elf_resolvefunction("mp_tls_alloc");
 *   TLS void *(*mp_tls_get)(int) = elf_resolvefunction("mp_tls_get");
 *   ptr = mp_tls_get(mp_tls_alloc(sz));
 *
 * Each call site guards the call with `if (ptr == NULL)` so the resolve
 * overhead (elf_resolvefunction is a symbol-table lookup) happens at most once
 * per module init.  Cast the returned void * to the desired TLS pointer type
 * at the call site.
 */
static inline TLS void *intr_tls_alloc(int sz)
{
    int (*mp_tls_alloc)(int) = elf_resolvefunction("mp_tls_alloc");
    TLS void *(*mp_tls_get)(int) = elf_resolvefunction("mp_tls_get");
    return mp_tls_get(mp_tls_alloc(sz));
}

#endif /* SYSINTERRUPTS_TLS_UTIL_H */
