# Filesystem direction — NOT btrfs

**Decision (2026-06):** Cardinal; will *not* implement btrfs. The on-disk
filesystem is a **custom relational/object filesystem inspired by the `libs/kvs`
design**, not a port of an existing Linux filesystem.

This note exists because the older design material is misleading on this point:

- `notes/core/Cardinal; Design Notes.txt` lists a `SysBtrfs module_init` step in
  the boot/task-load sequence. That name is **obsolete** — read it as "the
  storage/filesystem service", which is `CoreStorage`, not btrfs.
- The detailed design already in `notes/servers/CoreStorage/Main.md` is in fact
  the custom design (log-structured, object store + tags + capabilities,
  copy-on-write, B-tree content). That document — not btrfs — is the intended
  direction. The "relational, kvs-inspired" framing in this note is the lens to
  read it through: objects with typed/tagged attributes and queryable
  relationships, rather than a POSIX directory tree.

## Why kvs-inspired

`libs/kvs` is the in-tree key/value store already used by `SysReg` (the
registry). Reusing its data-model thinking for the filesystem keeps one
coherent storage abstraction across the system (registry, app data banks, files)
and matches the object-browser UI model described in `Main.md` ("the user
interface is simply an object browser"). The tag/attribute store in `Main.md`
is the relational layer.

## Status

- `servers/CoreStorage/src/main.c` — `module_init` now brings up a block-device
  registry and an fs-provider registry (`storage_register_blockdev` /
  `storage_register_fsprovider`), probing each registered provider against block
  devices. (No longer an empty stub.)
- `drivers/cardfs/src/main.c` — first-cut object store, registers as a CoreStorage
  fs provider; see `cardfs-exploration.md`.
- `drivers/tarfs/src/main.c` — `module_init` is an empty stub (the initrd tar
  reader the kernel uses at boot is separate, in `kernel/`).
- No final on-disk format is implemented yet (cardfs is a flat-map exploration);
  `Main.md` is the spec to implement against, reconciled with the kvs model.

## Open design questions (decide before implementing — consequential)

These shape the whole system and should be settled deliberately, not improvised:

1. **The relational/query model.** What exactly is "relational" here — kvs-style
   typed attributes per object plus tag indexes (as in `Main.md`), or something
   with richer joins? What is the query API surface, and how does it map onto the
   `SysObj` object model and `SysReg`?
2. **kvs reuse vs. re-implementation.** Is the on-disk structure literally `kvs`
   serialised (with COW + journaling layered on), or a separate B-tree store that
   merely borrows kvs's conceptual model? `libs/kvs` is currently in-memory.
3. **CoreStorage service interface.** The block-device/driver registration API
   (how `ahci`/USB-mass-storage attach), and the higher-level object/file API
   exposed to servers and userspace — this is the same "socket/port API" class of
   decision called out for CoreNetwork; the two should share a philosophy.
4. **Paging / persistence integration.** `Main.md` describes the disk backing
   transparent persistence via the VM system ("near limitless RAM"). That couples
   the filesystem to `SysVirtualMemory`/`SysMemory` and is a large commitment.

## Recommendation

Implement bottom-up and reviewably: first a CoreStorage block-device
registration interface + a read-only mount of the simplest real format (or keep
tarfs as the throwaway test backend), then the object store, then tags/queries.
Land the relational/query API as an explicit, separately-reviewed proposal
before wiring it into `SysObj`/userspace, since it is hard to change later.
