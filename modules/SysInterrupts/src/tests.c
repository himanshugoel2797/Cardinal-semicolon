// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdbool.h>
#include <cardinal/cs_error.h>

#include "SysTest/test.h"
#include "SysInterrupts/interrupts.h"

static void dummy_handler(int irq) {
    (void)irq;
}

static void test_allocate(test_ctx_t *ctx) {
    // base = 0 forces the dynamic search path (a non-zero *base would first try
    // a fixed allocation at that vector). The allocator hands out a contiguous
    // run starting at the first free vector >= 32 (vectors 0..31 are reserved
    // for x86 CPU exceptions).
    int base = 0;
    cs_error err = interrupt_allocate(4, interrupt_flags_exclusive, &base);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    // Minimum valid hardware vector is 32; the dynamic search never returns less.
    TEST_CHECK(ctx, base >= 32);

    if (err == CS_OK && base >= 32) {
        // The whole contiguous block [base, base+4) must really have been
        // reserved: a fixed exclusive re-allocation of any vector inside it must
        // now fail (it is marked blocked).
        for (int i = 0; i < 4; i++) {
            int probe = base + i;
            cs_error taken = interrupt_allocate(
                1, interrupt_flags_fixed | interrupt_flags_exclusive, &probe);
            TEST_CHECK_MSG(ctx, taken != CS_OK,
                           "allocated vector reports as already taken");
        }
    }
}

static void test_cpu_idx_inline(test_ctx_t *ctx) {
    // interrupt_get_cpu_idx() returns this core's APIC id (a unique, non-negative
    // identifier). It is not a logical 0..corecount-1 index, so we only assert it
    // is non-negative here; distinctness across cores is covered by the per-CPU
    // test below.
    int idx = interrupt_get_cpu_idx();
    TEST_CHECK(ctx, idx >= 0);
}

// Shared across the per-CPU tasks: a 256-bit "seen" bitmap of APIC ids. Each
// core atomically sets the bit for the id it observes; if a bit is already set
// when a core tries to set it, two cores returned the same id -- the bug this
// test exists to catch. APIC ids are unique, so no two cores legitimately
// target the same bit and there is no data race on the same word's value.
#define APIC_ID_WORDS 4 // 4 * 64 = 256 possible APIC ids
static volatile uint64_t g_seen_apic[APIC_ID_WORDS];

static void test_cpu_idx_percpu(test_ctx_t *ctx) {
    int id = interrupt_get_cpu_idx();
    TEST_CHECK(ctx, id >= 0);
    if (id < 0 || id >= APIC_ID_WORDS * 64)
        return;

    uint64_t bit = (uint64_t)1 << (id % 64);
    uint64_t prev = __atomic_fetch_or(&g_seen_apic[id / 64], bit, __ATOMIC_SEQ_CST);
    // This core's APIC id must not have been claimed by another core already.
    TEST_CHECK_MSG(ctx, (prev & bit) == 0,
                   "each core reports a distinct APIC id");
}

// Set by the handler registered in test_routing_task when the self-IPI it sends
// is actually delivered to this core and dispatched. volatile so the bounded
// wait loop below re-reads it from memory every pass rather than caching it in a
// register (the write happens in interrupt context, asynchronously to the loop).
static volatile int g_routing_fired;

static void routing_handler(int irq) {
    (void)irq;
    g_routing_fired = 1;
}

// End-to-end routing: allocate a vector, register a handler, send a self-IPI to
// this very core, and prove the handler ran. This is the only test that
// exercises the *dispatch* path (idt_mainhandler -> interrupt_funcs[vec]),
// closing the "nothing verifies a signal reaches a registered handler" gap.
//
// SAFETY -- why this is a TEST_RUN_TASK and not INLINE:
//  * Delivery requires interrupts to be ENABLED so the core can actually take
//    the self-IPI. An INLINE test runs in the runner thread with no guarantee
//    about IF, and must never wait-on-interrupt; a TEST_RUN_TASK runs as a
//    scheduled kernel task, which by construction runs with interrupts enabled
//    (the scheduler itself depends on timer interrupts), making delivery safe.
//  * The handler does NOT need to issue an EOI: idt_mainhandler sends the EOI
//    itself for every vector >= 32 (see idt.c), so there is no half-acked IPI
//    that could wedge the local APIC.
//  * The wait is BOUNDED. If delivery somehow never happens we fall out of the
//    loop and fail the assertion -- we never spin forever, so a regression here
//    fails the test instead of hanging CI.
//  * Self-IPI uses fixed delivery mode (ipi_delivery_mode_fixed) to this core's
//    own APIC id (interrupt_get_cpu_idx() returns the APIC id, which is exactly
//    the destination field interrupt_sendipi writes), i.e. a normal fixed
//    interrupt at the chosen vector -- not an INIT/STARTUP IPI.
static void test_routing_task(test_ctx_t *ctx) {
    int vec = 0;
    cs_error err = interrupt_allocate(1, interrupt_flags_exclusive, &vec);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    TEST_CHECK(ctx, vec >= 32);
    if (err != CS_OK || vec < 32)
        return;

    g_routing_fired = 0;
    interrupt_register_handler(vec, routing_handler);

    int self = interrupt_get_cpu_idx();
    interrupt_sendipi(self, vec, ipi_delivery_mode_fixed);

    // Bounded wait: the IPI is delivered asynchronously once we are interruptible.
    // ~10M iterations is generous on any emulated/real core yet still finite, so
    // a broken dispatch path fails the assert below instead of hanging forever.
    for (int spins = 0; spins < 10000000 && !g_routing_fired; spins++)
        __asm__ volatile("pause");

    TEST_CHECK_MSG(ctx, g_routing_fired,
                   "self-IPI was routed to the registered handler");

    interrupt_unregister_handler(vec, routing_handler);
}

static void test_register_unregister(test_ctx_t *ctx) {
    int base = 0;
    cs_error err = interrupt_allocate(1, interrupt_flags_exclusive, &base);
    TEST_CHECK_EQ_U(ctx, err, CS_OK);
    TEST_CHECK(ctx, base >= 32);
    if (err != CS_OK || base < 32)
        return;

    // A registered handler occupies one of IDT_HANDLER_CNT (16) slots for the
    // vector; unregister must free it again. Cycle register/unregister far more
    // times than there are slots: if unregister did not actually reclaim the
    // slot, the handler table would fill and interrupt_register_handler would
    // PANIC ("Interrupt oversubscribed!"), failing the run. Surviving the loop
    // proves the slot is reusable.
    for (int i = 0; i < 64; i++) {
        interrupt_register_handler(base, dummy_handler);
        interrupt_unregister_handler(base, dummy_handler);
    }
    TEST_CHECK_MSG(ctx, true,
                   "handler slot reusable across many register/unregister cycles");
}

void sysinterrupts_register_tests(void) {
    if (!test_mode_active())
        return;

    test_def_t allocate = {
        .suite = "SysInterrupts",
        .name = "allocate",
        .fn = test_allocate,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&allocate);

    test_def_t cpu_idx_inline = {
        .suite = "SysInterrupts",
        .name = "cpu_idx_inline",
        .fn = test_cpu_idx_inline,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&cpu_idx_inline);

    test_def_t cpu_idx_percpu = {
        .suite = "SysInterrupts",
        .name = "cpu_idx_percpu",
        .fn = test_cpu_idx_percpu,
        .run = TEST_RUN_PERCPU,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&cpu_idx_percpu);

    test_def_t register_unregister = {
        .suite = "SysInterrupts",
        .name = "register_unregister",
        .fn = test_register_unregister,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&register_unregister);

    test_def_t routing = {
        .suite = "SysInterrupts",
        .name = "routing_self_ipi",
        .fn = test_routing_task,
        .run = TEST_RUN_TASK,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&routing);
}
