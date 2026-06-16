# cardfs — object-store exploration (status)

`drivers/cardfs/src/main.c` is a **minimal, working exploration** of the
relational/object filesystem direction (see `filesystem-direction.md`). It is
NOT the final design — it's the simplest expression of the "objects with keys"
model, built to exercise the on-disk persistence path end-to-end on real
hardware before committing to the full log-structured/COW design.

## What works (verified, QEMU/KVM, xHCI → usb-storage)

Full vertical slice, validated:

```
USB host controller (UHCI or xHCI)
  -> usb_storage (Bulk-Only Transport + SCSI READ(10)/WRITE(10))
    -> CoreStorage block device (storage_blockdev_read/write)
      -> cardfs (format / put(key,data) / get(key))
```

Self-test output: `put` "greeting"/"answer", then `get greeting -> 'hello
cardinal'`, `get answer -> 'forty-two'` — persisted and retrieved from the disk
image.

On-disk layout: superblock @ LBA0; object table (128-byte entries) @ LBA1..8;
bump-allocated data blocks after. `put` writes data blocks + a table entry;
`get` scans the table by key and reads the data blocks.

## Findings from building it

- **WRITE(10) (bulk OUT) works** — this was the first exercise of the
  storage write path; reads alone had been tested before.
- **Fixed a real libc bug**: `common`'s `strncmp` compared all `n` bytes instead
  of stopping at the NUL, so key lookups (`strncmp(entry.key, "greeting", 64)`)
  never matched. Now standard. (Committed separately.)

## How it maps to the real design (and what's deliberately missing)

The real design (`CoreStorage/Main.md`) is a log-structured, copy-on-write object
store with tags/capabilities and B-tree content. cardfs is the flat-map stepping
stone. Deliberately absent, to add when the real design lands:

- **No log structure / COW / journaling** — `put` overwrites in place and bump-
  allocates data; there is no crash consistency.
- **No free-space management** — data blocks are never reclaimed (no delete).
- **No tags / relations / capabilities** — it's a flat key→blob map, not yet the
  relational/tag model. The "relational" layer is the next thing to prototype
  (e.g. a tag index object pointing at object ids, kvs-style).
- **No B-tree** — linear table scan; fine for tens of objects, not thousands.
  B-trees are now free to use (no licensing/patent constraint), so the real
  design's B-tree content/index structures can be adopted directly.
- **No userspace API** — cardfs registers as a CoreStorage fs provider
  (`storage_register_fsprovider`) and its probe mounts an existing volume, but it
  is not yet wired to a `SysObj`/syscall surface.
- **512-byte block / single device** assumptions; key length capped at 64.

## Done since the first cut

- **Split into its own module** (`drivers/cardfs`), out of `CoreStorage`.
- **FS-provider registration API** (`storage_register_fsprovider`): CoreStorage
  probes each registered provider per block device (synchronously at
  registration), replacing the old poll-the-list-and-settle self-test task.
- **Non-destructive by default**: the probe mounts an existing cardfs volume and
  otherwise declines; the format/roundtrip exploration is behind `CARDFS_SELFTEST`
  (off), so it never formats a real disk on boot.

## Suggested next steps

1. Replace the flat table with the `Main.md` object-store-table + a tag object
   (the relational layer) — still on top of the same block device.
2. Wire the provider to a `SysObj`/syscall surface so userspace can do file I/O.
3. Crash consistency (journal/COW) once the structure is settled.
