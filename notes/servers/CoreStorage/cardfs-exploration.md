# cardfs — crash-consistent object store (status)

cardfs lives in **`lisp/servers/cardfs.clp`**. It is a small, **robust** object
store — a flat key→blob map — that exercises the on-disk persistence path with
real crash-consistency and integrity, not the full relational design yet (see
`filesystem-direction.md` / `Main.md`). It was rewritten from an in-place
flat-table version into a **log-structured / copy-on-write** design so that:

- a power loss can never leave a half-applied mutation visible (**crash-atomic**), and
- bit-rot / torn writes are **detected** (crc32), never silently served as data.

## On-disk format (512-byte blocks, single device)

```
LBA 0, LBA 1 : two superblock "checkpoint" slots (A / B). Each is a complete
               root: {magic "CARDLOG1", version, block_size, generation,
               log_start, log_head, next_id, obj_count, crc32}. The slot with the
               HIGHEST generation whose crc32 validates is the live root.
LBA 2 ..     : an append-only LOG of records. A record is
               [header block | payload block ...]; the header holds
               {magic "CREC", type(put|tombstone), id, size, nblocks, keylen, key,
                payload_crc32, header_crc32}.
```

## Why it survives crashes (the whole point)

A mutation is two steps:

1. **Append** the record (header + payload) into free space *past* `log_head`.
   The live superblock doesn't reference it yet, so a torn write there is
   invisible — the log is append-only and never overwrites live data.
2. **Commit**: write a new superblock (generation+1, advanced `log_head`) into the
   *inactive* of the two slots. This single block write is the atomic commit.

So a crash is **all-or-nothing**:

- Torn write to the record → the old superblock still wins; the put never happened.
- Torn write to the commit superblock → its crc32 fails, mount falls back to the
  other slot (the pre-commit state, fully intact).
- Bit-rot in a committed payload → caught by `payload_crc32` on `get` → reported
  `corrupt`, never returned as good bytes.
- Bit-rot in a record header → caught by `header_crc32` during replay.

On **mount** we read both superblock slots, pick the newest valid one, and
**replay** the log `[log_start, log_head)` to rebuild an in-RAM index
(`key → data-lba, size, payload-crc, id`): a put record (re)binds a key, a
tombstone unbinds it (last-writer-wins). `format` invalidates slot B then writes
a fresh slot A, so a stale higher-generation checkpoint can't resurrect. Because
the block size is a fixed constant (512) rather than read from the superblock and
used as a divisor, a corrupt-but-magic-valid superblock can never cause a
divide-by-zero (the old design's silent provider-context death).

`crc32` (IEEE, table-driven) is implemented in pure Lisp — no new VM primitive.

## I/O API (CoreStorage message protocol)

Block I/O is by message (a provider sends `(read name lba count self)` to the
storage handle and gets `(complete status bytes)` back). The object API the store
exposes (sent to the cardfs provider handle):

| Message | Reply |
|---|---|
| `(probe name bsize bcount driver storage)` | auto-mount a valid volume + `(claim name)`, else decline (read-only) |
| `(format storage name bcount reply)` | `(complete ok\|io-error)` |
| `(put storage name key data reply)` | `(complete ok\|full\|io-error\|no-volume)` |
| `(get storage name key reply)` | `(got ok bytes \| miss #f \| corrupt #f \| no-volume #f)` |
| `(get-range storage name key off len reply)` | `(got ok bytes \| miss #f \| range #f \| corrupt #f \| no-volume #f)` |
| `(delete storage name key reply)` | `(complete ok\|miss\|io-error\|no-volume)` |
| `(stat storage name key reply)` | `(stat (size id) \| #f \| no-volume)` |
| `(keys storage name reply)` | `(keys (key ...) \| no-volume)` |

API design notes:

- **Corruption is a distinct outcome.** `get` returns `corrupt` (crc mismatch or a
  failed block read), not a silent `#f`, so a caller can tell *data loss* from a
  *miss*. This is the integrity contract; everything else builds on it.
- **`get-range` trades integrity for not materializing a whole blob.** A partial
  slice can't be checked against the whole-payload crc, so a ranged read is *not*
  integrity-verified (only a failed block read surfaces as `corrupt`). Use `get`
  when you need the guarantee. (A future per-block crc array would let ranged reads
  verify too.)
- **`delete` is a tombstone**, so it's crash-atomic like `put` and makes `put`
  (replace) well-defined; `stat`/`keys` are O(1)/O(n) over the in-RAM index.
- The provider is a single restricted context (no capabilities; all I/O rides a
  storage handle a message carries) and serialises overlapping requests by
  deferring any message that lands during an in-flight block I/O.

## Tested (host harness + in-OS)

`libs/lisp/test/test_cardfs.c` drives a **fault-injecting** RAM disk (peek / poke
a block, drop a specific write) through the real corestorage + cardfs, then
re-mounts a fresh provider (a "reboot") and asserts recovery:

- clean round-trip incl. a multi-block object, replace (last-writer-wins), `stat`,
  `keys`, `get-range` (incl. out-of-range), `delete`;
- a **lost commit superblock write** rolls the put back entirely (atomicity);
- **bit-rot** in a data block → `corrupt`, the neighbour object unaffected;
- a **destroyed active superblock** falls back to the older valid checkpoint;
- **both superblocks corrupt** → the volume declines to mount (no crash);
- a **full device** → `full`, consistent across remount.

The in-OS `check_cardfs` (SysLisp) mirrors the put/get/delete + reboot-replay path
over the scheduler. QEMU can't be killed mid-write reliably, so the host
fault-injection tests are the authoritative crash-consistency coverage.

## Deliberately absent (future, not this slice)

- **Log compaction / GC.** The log grows until the device is full (then `put`
  returns `full`); dead records (replaced/deleted) are never reclaimed. Compaction
  is itself a clean atomic step (write a fresh compacted log into free space, flip
  the superblock) and is the obvious next addition.
- **Write barrier / cache flush.** There is no flush primitive; we rely on the
  storage path completing each block write before its ack (`blk-write` waits for
  `complete`), which orders a record before its commit superblock. A device that
  reorders writes across that ack could, worst case, truncate the log at a torn
  record on replay — it still never serves corrupt data. A forward-linked /
  globally-checksummed record stream would let replay resync past a torn record.
- **Relational layer** (tags / typed attributes / queries), **multi-device**,
  **userspace SysObj/syscall surface**, **>64-byte keys / non-512 blocks**, and
  the transparent-persistence VM coupling from `Main.md`.

## Suggested next steps

1. Log compaction (atomic, as above) to bound disk + mount-replay cost.
2. Wire the message API to a `SysObj`/syscall surface for userspace file I/O.
3. Layer the `Main.md` object-store-table + tag objects (the relational layer) on
   top of this same crash-safe log.
