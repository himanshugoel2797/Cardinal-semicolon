# SysTaskMgr / SysUser / SysDebug / SysGdb

API reference for the Cardinal; scheduler (`SysTaskMgr`), syscall surface
(`SysUser`), in-memory debug log + serial mux (`SysDebug`), and the GDB
remote-serial-protocol stub (`SysGdb`).

## `task_create_kernel`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 6b2e414afc324614

Allocate a kernel task (round-robined onto a scheduler core) and return its id, without starting it.

**Parameters**
- `name` — task name (copied into the descriptor, truncated at 256 bytes).
- `perms` — `task_permissions_kernel` for a kernel task, or `task_permissions_none`
  for one that drops to user mode (the latter gets a syscall state allocated up
  front).
- `id` — out: the freshly allocated `cs_id`.

**Returns** `CS_OK`, or `CS_OUTOFMEM` if the descriptor or its address space could
not be allocated. The task starts in `task_state_uninitialized`; call
`task_start_kernel` to make it runnable. Placement is via the internal
round-robin (`pick_target_core`); while still single-core every task lands on the
BSP.

---

## `task_create_kernel_oncore`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** c50d345b6fe85ad2

Like `task_create_kernel` but pins the new task to a specific scheduler core.

**Parameters**
- `name`, `perms`, `id` — as in `task_create_kernel`.
- `core` — the target core's **sequential run-queue index** (`0 ..
  task_corecount()-1`), *not* its sparse APIC id. An out-of-range value
  (`core < 0 || core >= registered_cores`) falls back to normal round-robin
  placement rather than failing.

**Returns** `CS_OK` or `CS_OUTOFMEM`. Used e.g. to fan a per-CPU task onto every
online core.

---

## `task_corecount`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** b46d7108e15084e9

Return the number of cores that have joined the scheduler so far.

Each owns run-queue index `0 .. task_corecount()-1`. The count grows as
application processors come online (after `task_release_aps`), so it is the valid
upper bound for the `core` argument to `task_create_kernel_oncore`.

---

## `task_start_kernel`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** ebff6b4a7bd086f3

Make a previously-created task runnable with an entry handler and argument.

**Parameters**
- `id` — task to start.
- `handler` — entry point. For a kernel task it is called as `handler(arg)`; when
  it returns the task is auto-ended and yields forever. For a `task_permissions_none`
  task this is the user-mode entry RIP; a user stack is allocated and mapped at
  `0x100000000` and the task is set up to `sysret` into it.
- `arg` — opaque argument passed to a kernel handler.

**Returns** `CS_OK` on success; `CS_UNKN` if `handler` is `NULL` or no task with
`id` exists. Sets the task to `task_state_pending`.

---

## `task_end_kernel`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** b799abdbd69ab5e7

Mark a task as exited so the scheduler reaps it.

**Returns** `CS_OK`, or `CS_UNKN` if no task with `id` exists. Sets the task to
`task_state_exited`; the actual teardown (stacks, register/FPU state, descriptors,
address space) happens on a later scheduler pass once no core is still on the
task's kernel stack. Ending the currently-running task does not by itself switch
away — call `task_yield` after if you must not return.

---

## `task_yield`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 6303cc7e65ac4e3b

Cooperatively yield this core to the next runnable task.

Saves the caller's full register state with inline asm, runs the scheduler's
select-next, and `iretq`s into the chosen task; control resumes here when this
task is next scheduled. Interrupts are saved/disabled across the switch and
restored on return. Takes no arguments and returns no value.

---

## `task_current`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 254020e1fdfca256

Return the `cs_id` of the task currently running on this core.

Reads the per-core current task directly (no locking); valid only from a
scheduled context.

---

## `task_sleep`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** cb422c5c97af1054

Put a task to sleep for at least `ns` nanoseconds.

**Parameters**
- `id` — the task to sleep.
- `ns` — minimum sleep duration; the wake deadline is `timer_timestamp_ns() + ns`,
  re-checked by the scheduler's runnability test.

**Returns** `CS_OK`, or `CS_UNKN` if no such task (in which case it deliberately
does *not* yield — it must not deschedule an unrelated current task).

**Caveat — deschedule behaviour.** When `id` is *this core's* running task,
`task_sleep` yields so the core actually switches away. But the yield only takes
effect if interrupts can flow: a caller already holding `cli()` (e.g. AHCI init)
keeps running, merely mislabelled `task_state_sleep`, until the next preemption
tick — which never comes under `cli()`. Such code must busy-spin via the
TSC-calibrated `SysTimer` waits instead. Sleeping a task *other* than the running
one only marks it; this core keeps running.

---

## `task_monitor`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 34d70a90be6f0ffe

Suspend a task until a 32-bit memory location changes away from a known value, then yield.

**Parameters**
- `id` — task to suspend.
- `tgt` — pointer (in the task's address space) to the `uint32_t` to watch;
  resolved to a physical address and remapped uncached for the scheduler to poll.
- `cur_val` — the value considered "unchanged"; the task becomes runnable again
  once `*tgt != cur_val`.

**Returns** `CS_UNKN` (and immediately if `tgt` is `NULL`). The task enters
`task_state_suspended_monitor_mem_32` and this call then `task_yield`s. This is
the primitive `semaphore_wait` is built on.

---

## `task_virttophys`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** ae9f5ddeeaae6034

Translate a virtual address in a task's address space to its physical address.

**Parameters** `id` — owning task; `vaddr` — address to translate; `phys` — out:
the physical address.

**Returns** `CS_OK` on a successful translation; `CS_UNKN` if `phys` is `NULL`,
the task does not exist, or the address is unmapped.

---

## `task_map`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 864121de2f3332a1

Allocate and map a memory region into a task's address space, returning a descriptor id.

**Parameters**
- `id` — owning task.
- `name` — region name (required for shared mappings).
- `vaddr`, `sz` — virtual address and size of the mapping.
- `flags` — `task_map_flags_t` (`task_map_oneway` / `task_map_shared` /
  `task_map_oneuse`).
- `owner_perms`, `child_perms` — `task_map_perms_t` cache/access permissions for
  the owner and any future child.
- `child_count` — number of children allowed.
- `shmem_id` — out: the descriptor id for the new mapping.

**Returns** `CS_OK`; `CS_UNKN` for invalid argument combinations (`shmem_id ==
NULL`; a shared mapping with no `name`; `oneway`/`oneuse` without `shared`).
**Caveat:** only the private (non-`shared`) path is implemented — any mapping with
`task_map_shared` set currently `PANIC`s ("Shared memory not implemented"). The
private path allocates zeroed physical memory and maps it with the requested
cache/permission flags.

---

## `task_updatemap`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** c8fcdd2a74a24352

Re-apply (possibly reduced) permissions to an existing mapping and shoot down stale TLBs.

**Parameters** `id` — owning task; `shmem_id` — descriptor from `task_map`;
`perms` — new permissions, masked by the descriptor's owner/child permission set.

**Returns** `CS_OK` (a no-op if the descriptor is not a map entry). After dropping
the task lock it issues a cross-core `vmem_shootdown` so the tightened permissions
are enforced on every core, not just the editing one.

---

## `task_unmap`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 1737fc55be217911

Unmap a mapping descriptor, shoot down stale TLBs, and free its backing frames if owner.

**Parameters** `id` — owning task; `shmem_id` — descriptor from `task_map`.

**Returns** `CS_OK` (a no-op if the descriptor is not a map entry). The cross-core
`vmem_shootdown` runs with interrupts on and no lock held, and completes *before*
`physmem_free` returns the frame for reuse so no core can touch it through a stale
TLB entry.

---

## `task_allocdescriptor`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 58f1031e7b45e236

Register a resource descriptor with a free-action callback in a task's descriptor table.

**Parameters**
- `id` — owning task.
- `action` — `DescriptorResourceFreeAction` (`void (*)(void *)`) invoked with
  `state` when the descriptor is freed or the task is torn down.
- `state` — opaque pointer passed to `action` (nullable).
- `descriptor` — out: the allocated descriptor id (nullable).

**Returns** `CS_OK`, or `CS_UNKN` if `action` is `NULL`.

---

## `task_freedescriptor`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 245d7778cb4a02b5

Free a resource descriptor, invoking its registered free-action.

**Parameters** `id` — owning task; `descriptor` — id from `task_allocdescriptor`.

**Returns** `CS_OK` (a no-op if the descriptor is not a resource entry). Calls the
descriptor's `action(state)` and releases the slot.

---

## `semaphore_init`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** 86bf202bedb7d75e

Initialise a counting semaphore to count 0 with its spinlock cleared.

Takes a `semaphore_t *`. Must be called before any `semaphore_signal` /
`semaphore_wait`.

---

## `semaphore_signal`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** f102ce11cdebe143

Increment a semaphore's count under its spinlock (the "post" / "up" operation).

Takes a `semaphore_t *`. A waiter blocked in `semaphore_wait` (via
`task_monitor`) wakes when the count moves off zero.

---

## `semaphore_wait`

- **kind:** function
- **lang:** c
- **source:** `modules/SysTaskMgr/src/task.c`
- **hash:** d2e39e84d977e1b7

Block until a semaphore's count is positive, then decrement it (the "down" operation).

Takes a `semaphore_t *`. While the count is zero it arms a `task_monitor_noyield`
on the count word and `task_yield`s, re-checking after each wake — so a waiter
descheduled cleanly rather than busy-spinning the CPU.

---

## `syscall_sethandler`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 1dc5b9366c00f22a

Install a syscall handler function into the default syscall set at index `idx`.

**Parameters** `idx` — syscall number; `func` — handler (or `NULL` to clear).

**Returns** `CS_OK` if `0 <= idx < SYSCALL_COUNT` (256); otherwise `CS_UNKN`
without touching the table — the bounds check rejects negative and out-of-range
indices. The default set (`syscall_set_table[0]`) is what
`syscall_getdefaultstate` wires every new task to.

---

## `syscall_set_syscallset`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** ed8fdc4a0aecf9c7

Install a syscall-set table pointer at set index `idx` for the current core's state.

**Parameters** `idx` — set index; `set` — pointer to a `void *[SYSCALL_COUNT]`
table (may be `NULL`).

**Returns** `CS_OK` if `0 <= idx < SYSCALL_SET_COUNT` (128); otherwise `CS_UNKN`.
A syscall selects its function via `syscall_set_table[r13][r12]`; the entry asm
panics on an out-of-range set/call index or a null set/handler.

---

## `syscall_get_syscallset`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 20c299f9a5c57684

Return the syscall-set table pointer installed at set index `idx`.

**Returns** the stored `void **` if `0 <= idx < SYSCALL_SET_COUNT`; otherwise
`NULL`. The same bounds check guards the read as the write.

---

## `syscall_touser`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 5e5d92f344be70fc

Transition the current task into user mode using its saved syscall state.

**Parameters** `arg` — the stack base / argument forwarded to the user transition.
Restores the per-core syscall register state and `sysret`s to user mode; does not
return on the kernel side. Used by `task_start_kernel` as the kernel-to-user
entry trampoline.

---

## `syscall_getfullstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 673e8f06b3d8251c

Snapshot the current core's full syscall state (kernel stack, registers, all syscall sets) into `dst`.

`dst` must point at a buffer of at least `syscall_getfullstate_size()` bytes. The
scheduler calls this to save a task's syscall state on a context switch. Copies
field-by-field (a struct copy through the `%gs` segment pointer cannot be lowered
by the backend).

---

## `syscall_setfullstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 53d7e4817ece4a1b

Restore a previously-saved syscall state into the current core's live state.

`state` is a buffer produced by `syscall_getfullstate`. The scheduler calls this
when switching *to* a task. The inverse of `syscall_getfullstate`.

---

## `syscall_getdefaultstate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 9afd281c865065d6

Initialise a syscall state buffer for a brand-new task with default registers and the default syscall set.

**Parameters** `state` — buffer to fill; `kernel_stack` — the task's kernel stack;
`user_stack` — initial user RSP/RBP; `rip` — initial user entry point. Sets
`rflags = 0x3200`, zeroes the callee-saved registers, points set 0 at the global
`syscall_funcs` table, and nulls sets 1..N.

---

## `syscall_getfullstate_size`

- **kind:** function
- **lang:** c
- **source:** `modules/SysUser/src/platform/x86_64/syscall.c`
- **hash:** 2712b839431588ee

Return the byte size of the full syscall-state structure.

`PURE`. Used by `SysTaskMgr` to allocate the per-task `syscall_data` buffer that
`syscall_getfullstate` / `syscall_setfullstate` read and write.

---

## `debug_getlogbase`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/debug_log.c`
- **hash:** 614203cd2b1def7f

Return a pointer to the base of the in-memory debug log ring buffer.

The buffer is a fixed 32 KiB (`DEBUG_LOG_LEN`) circular array that `log()`
appends to. Combine with `debug_getlogendoffset` to read out the most recent log
text. The console/serial output path is separate (`print_str` / `DEBUG_PRINT`).

---

## `debug_getlogendoffset`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/debug_log.c`
- **hash:** 3bc080b2cf0f6705

Return the current write cursor (end offset) into the debug log ring buffer.

The cursor wraps modulo the 32 KiB buffer length, so a reader must treat the
buffer returned by `debug_getlogbase` as circular.

---

## `csmux_set_transport`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** ac62da0a1eb556ad

Replace CSMUX's active byte transport (default: polled COM1) with a pluggable one.

**Parameters** `t` — a `csmux_transport_t` (`write` + non-blocking `getb` +
`state`); the descriptor is copied. Ignored if `t` or either callback is `NULL`.
Call before `csmux_activate` (e.g. from a USB-serial driver probe). Installing a
custom ("heavy") transport arms the same-core re-entrancy guard on the TX lock.
Neither callback may emit to the debug log, or it would re-enter `csmux_send`
under its own lock.

---

## `csmux_activate`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** e08b0401b5f7d4d9

Switch the active serial link into framed (multiplexed) mode.

Emits a raw `[[CSMUX-START v1]]` banner first so the host demuxer has an
unambiguous raw-to-framed sync point (re-printed after each reboot), then sets the
active flag inside the TX lock so there is no window where the banner is sent but
the flag is clear. Idempotent. After this, `print_str` routes the log onto
`CSMUX_CH_LOG`. CSMUX is dormant on a normal boot — this is only called for a
harness-driven test run.

---

## `csmux_active`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 6d61e08a570d204a

Return whether CSMUX framed mode is currently active.

`true` once `csmux_activate` has run. While `false`, the send/append/flush
entry points are no-ops and raw text flows to COM1 unframed.

---

## `csmux_xport_heavy`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 8a8789ce9411fcc5

Return whether a custom (non-default-COM1) transport has been installed.

`true` once `csmux_set_transport` has bound a custom link (e.g. a USB-serial
adapter). The test harness uses this to wait for an FTDI link to enumerate before
the handshake so the mux rides that single link.

---

## `csmux_log_append`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** a2fe4e4252260534

Append debug-log bytes to the coalescing buffer (channel `CSMUX_CH_LOG`).

The high-volume log is batched rather than framed per line — per-line transfer
latency over a USB link would starve the low-rate control/REPL channels sharing
the wire. The buffer auto-flushes when full. No-op until `csmux_active`. A
re-entrant call from inside a heavy transport's write is dropped to avoid
self-deadlock on the TX lock.

---

## `csmux_log_flush`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 275caa6a43cdb3e5

Flush any buffered debug-log bytes as one or more `CSMUX_CH_LOG` frames now.

Called periodically and before idling. No-op until `csmux_active`; a re-entrant
call from inside a heavy transport write is dropped.

---

## `csmux_send`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 78148ebc59780186

Send one whole frame atomically on logical channel `chan`.

**Parameters** `chan` — `CSMUX_CH_LOG` / `CSMUX_CH_CTRL` / `CSMUX_CH_REPL`; `buf`,
`len` — payload (`len <= CSMUX_MAX_PAYLOAD`, 1024).

**Returns** `0` on success; `-1` if CSMUX is inactive or `len` exceeds the max
payload; `-2` if it is a same-core re-entry on a heavy transport (dropped to avoid
self-deadlock). Busy-polled TX with no IRQ/DMA/malloc, so it is safe from
`cli()`/trap context — but a CPU exception taken *while this core already holds the
TX lock* must not re-enter `csmux_send` or it self-deadlocks. Buffered log bytes
are flushed first so control/REPL frames keep order with the log.

---

## `csmux_raw_write`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 27887619014979e1

Unframed write over the link, serialised by the same lock as `csmux_send`.

**Parameters** `buf`, `len`. Always writes to COM1 directly — never the (possibly
USB) custom transport — because this is the pre-activation boot log, which can be
emitted from inside the USB stack itself. Holding the TX lock keeps one core's raw
bytes from interleaving with another core's frame.

---

## `csmux_recv_byte_pump`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 5c0541440a4f72f5

Pump the receiver: read all currently-available bytes, run the de-framer, and route complete frames to per-channel rings.

Non-blocking. **Returns** the number of raw bytes consumed. Validates each frame's
length and CRC16-CCITT before pushing its payload; junk between frames (e.g. boot
text) is ignored, and frame overflow resyncs on the next `0x7E`. Serialised by the
RX lock so two cores pumping at once cannot corrupt the shared de-framer state.

---

## `csmux_chan_read`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 44d9d8e988927088

Drain up to `cap` already-received bytes on `chan` into `buf` (pumps first).

**Returns** the number of bytes copied (0 if none). Only `CSMUX_CH_REPL` has a
receive ring; `CH_LOG` and `CH_CTRL` are output-only and always return 0.

---

## `csmux_chan_avail`

- **kind:** function
- **lang:** c
- **source:** `modules/SysDebug/src/csmux.c`
- **hash:** 89c18c5ac903f6e7

Return the number of bytes currently waiting on `chan` without consuming them (pumps first).

Used by the REPL's non-blocking input poll. Returns 0 for the output-only
channels (`CH_LOG`, `CH_CTRL`).

---

## `gdb_stub_wait`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** 0d2f54291742b322

Drop into the GDB stub and wait for a debugger, raising a breakpoint.

Useful from a boot script to debug early boot. **Returns** `0` so it works as a
`CALL:` boot-script target.

> Note: on the current branch only the SysGdb header is present in-tree; the
> defining `.c` is not, so the freshness hash for the SysGdb entries will report
> `missing-source` until the implementation lands.

---

## `gdb_register_transport`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** b601d736820a92d9

Install a pluggable byte transport as the active GDB channel, replacing the default COM2.

**Parameters** `transport` — a `gdb_transport_t` (`getc`/`putc` blocking byte I/O,
optional non-blocking `poll` for async Ctrl-C break-in, and `state`); the
descriptor is copied, so the caller need not keep it alive. Lets a driver (e.g.
USB-serial) carry the GDB channel without any GDB-protocol knowledge.

---

## `gdb_unregister_transport`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** a6b9e91c9c12a5e9

Revert the GDB channel to the built-in COM2 transport.

**Parameters** `transport` — if non-`NULL`, the revert happens only when it is
still the active transport (so a stale unplug cannot clobber a newer one); pass
`NULL` to force the revert.

---

## `gdb_poll_breakin`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** 9ea014f54bd7d2cf

Poll the active transport for async break-in and, if a byte arrived, break into the stub.

A no-op when the active transport has no `poll`. **Returns** `0`. A transport with
no receive IRQ drives this from a polling loop; the break-in decision and the
entire RSP protocol stay owned by SysGdb, so the caller needs no GDB knowledge.

---

## `gdb_active_transport_getc`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** e32a99dac6dcd634

Test-only accessor: return the active transport's `getc` function pointer as an opaque `void *`.

Lets an in-OS test confirm a specific transport was actually installed. Not part
of the debugging API; drivers should not rely on it.

---

## `gdb_default_transport_active`

- **kind:** function
- **lang:** c
- **source:** `modules/inc/SysGdb/gdb.h`
- **hash:** 1e6f6db2164cc620

Test-only accessor: return whether the built-in COM2 transport is currently the active channel.

Lets an in-OS test confirm a revert to the default really happened. Not part of
the debugging API; drivers should not rely on it.
