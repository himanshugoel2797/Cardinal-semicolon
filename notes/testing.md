# In-OS testing: SysTest

Two complementary test layers exist:

- **Host unit tests** (`tests/`, run via `scripts/run-tests.sh`): exercise the
  freestanding-but-host-compilable leaf code (crypto, libc helpers)
  on the build machine. No kernel needed. Fast.
- **In-OS tests** (`modules/SysTest`): run *inside* the booted OS, so they can
  test things that need the real kernel — memory, the registry, the scheduler,
  drivers, SMP behaviour. This doc covers SysTest.

## How it works

`SysTest` is a kernel-privileged module loaded **very early** (right after
`SysDebug`, see `loadscript.txt`) so that essentially every other
module/server/driver is loaded *after* it and can register tests. It depends on
nothing but kernel/common symbols; the scheduler/MP entry points it needs to run
task/per-CPU tests are resolved dynamically at run time.

Execution is gated by the **kernel command line**: tests only run when the kernel
is booted with the `cardinal.test` flag (the bootloader passes it via the
multiboot2 cmdline tag; the kernel stashes it in `CardinalBootInfo.Cmdline` and
SysTest reads it). On a normal boot, registering tests costs nothing and
`test_run_all()` is a no-op.

`servicescript.txt` calls `test_run_all` once the whole service stack is up (just
after `task_release_aps`). In test mode it runs every registered test, prints
TAP-style results over COM1, and exits the machine via QEMU's `isa-debug-exit`
device. A completion sentinel (`[SysTest] ALL TESTS PASSED` / `TESTS FAILED`) is
printed for log scraping.

## Writing a test

In the module under test, include the public header and register from
`module_init`:

```c
#include "SysTest/test.h"

static void mymod_basic(test_ctx_t *ctx) {
    TEST_CHECK(ctx, some_init() == CS_OK);
    TEST_CHECK_EQ_U(ctx, lookup(42), 0xCAFE);
}

int module_init(void) {
    // ... normal init ...
    test_def_t t = {
        .suite = "MyMod", .name = "basic",
        .fn = mymod_basic, .run = TEST_RUN_INLINE,
    };
    test_register(&t);
    return 0;
}
```

Add `../inc` (i.e. `modules/inc`) to the module's include dirs if it isn't
already, so `SysTest/test.h` resolves.

### Triggers (`test_def_t.run`)

- `TEST_RUN_INLINE` (default) — runs synchronously in the runner thread. Use for
  pure logic / data-structure tests that never block.
- `TEST_RUN_TASK` — runs in its own kernel task. Use when the test must sleep,
  yield, or otherwise touch the scheduler.
- `TEST_RUN_PERCPU` — runs once on every online core (one pinned task each); the
  per-core result is aggregated. Read the current core via `test_core(ctx)`.

A module can also force its own suite to run synchronously at a chosen point with
`test_run_suite("MyMod")` (e.g. right after it finishes init); those tests are
then skipped by the later global sweep.

### Assertions

`TEST_CHECK`, `TEST_CHECK_MSG`, `TEST_CHECK_EQ_U`, `TEST_CHECK_NE_U`,
`TEST_CHECK_EQ_PTR`, `TEST_FAIL` — all record into `ctx` and **never abort the
run**, so one failing assertion (or test) doesn't take down the rest of the suite.

## Running

```bash
./scripts/build.sh                          # build kernel + modules
cmake --build build --target test-image     # -> build/ISO/os-test.iso
./scripts/run-tests-qemu.sh                  # boot it, run tests, report (exit 0 == pass)
```

`run-tests-qemu.sh` env knobs: `ACCEL` (default auto; CI uses `tcg`), `SMP`
(default 2 — per-CPU tests want >1), `TIMEOUT`, `MACHINE`, `MEM`, `LOG`. The
normal `image` target and `scripts/run-qemu.sh` are unaffected — a normal boot
never enters test mode.

## Death tests (failure tests that crash the kernel)

A **death test** asserts that some operation *kills* the kernel — an expected CPU
fault (null/bad deref → `#PF`, non-canonical access → `#GP`, bad opcode → `#UD`,
…) or a `PANIC`. This is exactly the class of failure a security-oriented
microkernel most wants pinned down, but the verdict can't survive in-guest: the
kernel just died. So the state is held **host-side** by a harness that talks to
the guest over serial and survives guest reboots.

Death tests are **local-only** — they are intentionally *not* run in web CI
(persistent-reboot under TCG is expensive). In a plain `cardinal.test` run (incl.
CI) every death test is reported `# SKIP (harness only)`.

### How it works

- **CSMUX** (`modules/SysDebug/src/csmux.c`, `<SysDebug/csmux.h>`): a tiny
  HDLC-style framing layer that multiplexes several logical channels over the one
  COM1 serial link — `ch0` debug log, `ch1` test control, `ch2` tunneled GDB.
  It is dormant on a normal boot (`print_str` writes raw text); it only switches
  on when the kernel is booted with the extra `cardinal.harness` cmdline token,
  and the host harness then demuxes the channels apart. This is what lets GDB
  keep working over the same single wire during a harness run (`ch2`; on real
  hardware that one wire is all you may have).
- **Reboot**: a death during an armed death test is caught in the fault path
  (`interrupt_set_death_hook`, SysInterrupts) and the PANIC path
  (`debug_set_trap_hook`, SysDebug); the hook reports `DIED vec=<n>` on `ch1` and
  reboots via `system_reset()` (0xCF9, then 8042, then triple fault).
- **Cursor across reboots**: the host harness
  (`scripts/systest-harness.py`) runs **one** persistent QEMU (no `-no-reboot`),
  holds the death-test cursor, answers the per-boot `HELLO` with `OLEH cursor=N`,
  records each death, and advances. A death test that *returns* (`SURVIVED`) or
  hangs (per-death timeout → forced `system_reset` via the QEMU monitor) is a
  failure. When the cursor passes the last death test the guest sends `ALLDONE`;
  the harness writes the aggregate sentinel into the log and exits 0/1.

### Writing a death test

```c
// fn must NOT return -- it triggers the death. `vec` is the expected CPU vector
// (e.g. 14 #PF, 13 #GP, 6 #UD) or TEST_DEATH_ANY for "any fault or PANIC".
static void mymod_oob_write(test_ctx_t *ctx) {
    (void)ctx;
    *(volatile uint32_t *)0xDEADBEEFDEADBEE0ull = 0; // non-canonical -> #GP
}

test_def_t d = TEST_DEATH_DEF("MyMod", "oob_write", mymod_oob_write, 13);
test_register(&d);
```

### Running locally

```bash
./scripts/build.sh
cmake --build build --target harness-image   # -> build/ISO/os-harness.iso
./scripts/run-deathtests.sh                  # builds the ISO if missing, drives the harness
```

`run-deathtests.sh` / `systest-harness.py` env knobs: `ACCEL` (default `tcg`;
`kvm` is much faster if available), `MACHINE`, `MEM`, `SMP`, `TIMEOUT` (overall),
`DEATH_TIMEOUT` (per-test), `LOG`, `GDB_PORT`. To debug a paused guest mid-run,
attach GDB to the tunneled `ch2` (see `notes/debugging-gdb.md`).

### Which serial link the mux rides (auto-detected)

CSMUX has a pluggable byte transport (`csmux_set_transport`, `<SysDebug/csmux.h>`).
It defaults to **COM1**, but on real hardware the only link is often a USB-serial
(FTDI) adapter. In harness mode, `drivers/usb_serial` detects the `cardinal.harness`
token and, on binding an FTDI adapter, registers it as the CSMUX transport — so the
whole mux rides that one link. The SysTest runner waits briefly for such a link to
enumerate before the handshake, then uses it; if none appears it stays on COM1.
Link selection is automatic; the only knob is the `cardinal.harness` token.

All three channels — log (ch0), control (ch1), GDB (ch2) — ride the one link, so
a board whose only serial is a USB-to-serial dongle gets everything over it. The
debug log is high-volume, so it is **coalesced**: log bytes are buffered and
flushed as a few large CH_LOG frames (auto-flush when full, before any control/GDB
frame, and via `csmux_log_flush`) instead of one USB transfer per line. Without
that, the per-transfer latency of a line-per-frame log over USB starves the
low-rate control channel and the handshake never completes.

Drive it from the host either against QEMU's emulated FTDI:

```bash
LINK=ftdi ./scripts/run-deathtests.sh
# or: python3 scripts/systest-harness.py --link ftdi ...
```

…or against a real adapter (no QEMU; boot the target with
`cardinal.test cardinal.harness`):

```bash
python3 scripts/systest-harness.py --serial-device /dev/ttyUSB0 --baud 115200
```

**Known QEMU limitation:** with QEMU's `-device usb-serial`, the mux-over-FTDI
handshake + first death work end-to-end, but the emulated adapter does not
re-enumerate after the guest's `0xCF9` platform reset, so subsequent death tests
fall back to COM1 within one QEMU session. A real platform reset re-signals USB
connect, so multi-death FTDI runs are expected to work on real hardware (the host
`--serial-device` mode is the path for that); over QEMU, use the COM1 link
(`run-deathtests.sh` default) for the full multi-death sweep.

CI runs this as a separate `test` job in `.github/workflows/build.yml` — the
first gate that actually boots the OS rather than only checking that artifacts
built.

> The `image`/`test-image` targets now depend on every `.celf` target (see the
> `COLLECT_CELF_TARGETS` block in the root `CMakeLists.txt`), so they re-stage all
> modules into the initrd on every build. Building `image` in isolation is safe;
> it no longer produces the partial initrd that used to panic the kernel with
> "Failed to find module".
