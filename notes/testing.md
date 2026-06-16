# In-OS testing: SysTest

Two complementary test layers exist:

- **Host unit tests** (`tests/`, run via `scripts/run-tests.sh`): exercise the
  freestanding-but-host-compilable leaf code (crypto, checksums, libc helpers)
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

CI runs this as a separate `test` job in `.github/workflows/build.yml` — the
first gate that actually boots the OS rather than only checking that artifacts
built.

> The `image`/`test-image` targets now depend on every `.celf` target (see the
> `COLLECT_CELF_TARGETS` block in the root `CMakeLists.txt`), so they re-stage all
> modules into the initrd on every build. Building `image` in isolation is safe;
> it no longer produces the partial initrd that used to panic the kernel with
> "Failed to find module".
