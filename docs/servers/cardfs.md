# cardfs

> A crash-consistent, integrity-checked object store that layers on top of [corestorage](corestorage.md) block devices, providing a flat key→blob map with CRC32-verified reads and log-structured, copy-on-write writes.

| | |
|---|---|
| **Source** | `lisp/servers/cardfs.clp` |
| **Kind** | server (filesystem provider) |
| **Bound by** | `lisp/init.clp` — always; called as `(start-cardfs storage)` before any block devices or USB storage register |
| **Registers with** | [corestorage](corestorage.md) via `(register-fsprovider 'cardfs prov)` on startup |
| **Capabilities** | none — runs as a `spawn-restricted` context with no imported `sys-*` modules; all block I/O is performed by messaging the `storage` handle passed to `start-cardfs` |

## Overview

cardfs is a single-context, log-structured filesystem provider. It maintains an in-RAM hash-table index (key → `(data-lba size payload-crc id)`) that is rebuilt on mount by replaying the on-disk append-only log. Every mutation appends a new record to free log space and then commits atomically by writing a new superblock into the inactive checkpoint slot; a crash can never leave a half-applied mutation visible.

cardfs never formats a volume implicitly. A `probe` message from corestorage reads the two superblock slots and claims the device only if a valid `CARDLOG1` volume is found; a `format` request must be sent explicitly before a blank device can be used.

All block I/O inside a request handler is performed synchronously (send-then-wait). Because cardfs is a single context, a new request that arrives while a block I/O round-trip is in flight is deferred into an internal `stash` list and processed after the I/O completes, serialising all concurrent callers through this one context.

The log grows without bound until the device is full; there is no log compaction or GC in the current implementation (a `put` on a full log returns `'full`).

## On-disk format

Block size is fixed at 512 bytes. All multi-byte integers are native-endian (matching the Lisp `bytes-uNN` primitives).

### Superblock slots (LBA 0 and LBA 1)

Two interchangeable checkpoint slots. On mount the slot with the highest `generation` whose trailing CRC32 validates is the live root; the other is the inactive slot and is overwritten on the next commit.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 bytes | Magic: `CARDLOG1` (bytes `67 65 82 68 76 79 71 49`) |
| 8 | u32 | Version (must be `1`) |
| 12 | u32 | Block size (must be `512`) |
| 16 | u64 | `generation` — monotonically increasing; higher wins on tie-break |
| 24 | u64 | `log_start` — first log LBA (always `2` in practice) |
| 32 | u64 | `log_head` — one past the last committed log block |
| 40 | u64 | `next_id` — next object ID to assign |
| 48 | u64 | `obj_count` — live object count |
| 508 | u32 | CRC32 (IEEE 802.3 / poly `0xEDB88320`) over bytes `[0, 507]` |

### Log records (LBA 2 onwards)

Each record is one header block followed by zero or more payload blocks.

**Header block** (one 512-byte block):

| Offset | Size | Field |
|--------|------|-------|
| 0 | u32 | Record magic: `0x43524543` (`"CREC"`) |
| 4 | u32 | Type: `1` = put, `2` = tombstone |
| 8 | u64 | Object ID |
| 16 | u64 | Payload size in bytes |
| 24 | u32 | Key length (capped at `KEY-LEN` = 64) |
| 28 | u32 | `nblocks` — number of payload blocks that follow |
| 32 | u32 | Payload CRC32 (whole payload; `0` for tombstones) |
| 40 | 64 bytes | NUL-padded key (`KEY-LEN` = 64 bytes maximum) |
| 508 | u32 | Header CRC32 over bytes `[0, 507]` |

Payload blocks immediately follow the header block; each is 512 bytes (the last may be zero-padded). A tombstone record has zero payload blocks (`nblocks` = 0, `size` = 0).

**Key truncation:** keys longer than 64 characters are silently truncated to their first 64 characters in both `put` and log replay. Index lookups use the truncated form, so a key `>64` chars written in one session is found under its truncated prefix on remount.

## Initialization

`init.clp` calls `start-cardfs` unconditionally before any block device or USB storage provider registers. The function spawns the provider context and registers it with corestorage so that every subsequently-registered block device is immediately probed.

```scheme
(start-cardfs storage)   ; storage = context handle returned by (start-storage-service)
                         ; → provider context handle (also returned to caller)
```

`start-cardfs` is the only exported symbol from the `cardfs` module.

## Message protocol

Messages are sent directly to the cardfs provider context handle (returned by `start-cardfs` and also retrievable as the second element of the `register-fsprovider` message corestorage logged). All requests carry an explicit `reply` context handle; cardfs uses `reply-to` to send the response.

### `probe`

Sent **by corestorage**, not by application code. cardfs replies by sending `(claim name)` back to the storage handle if the device holds a valid CARDLOG1 volume.

- **Request:** `(probe name bsize bcount driver storage)`
  - `name` — device name string (e.g. `"ahci0"`)
  - `bsize` — block size reported by the driver (informational; cardfs always uses 512)
  - `bcount` — total block count of the device
  - `driver` — driver context handle (passed through; not used by cardfs directly)
  - `storage` — corestorage context handle (cardfs sends `(claim name)` here on success)
- **Reply:** none to the sender. On success, sends `(claim name)` to `storage`. On failure (no valid superblock), logs and continues.

### `format`

Erase and initialise a device as a fresh CARDLOG1 volume. Writes a zeroed slot B then a fresh generation-1 superblock to slot A. Not crash-atomic (a crash mid-format leaves either the old volume or no volume, never a torn mix).

- **Request:** `(format storage name bcount reply)`
  - `storage` — corestorage block I/O handle
  - `name` — device name string
  - `bcount` — total block count
  - `reply` — context to reply to
- **Reply:** `(complete ok)` or `(complete io-error)`

```scheme
(send cardfs-ctx (list 'format storage "ahci0" 16384 (self)))
;; recv -> ('complete 'ok) or ('complete 'io-error)
```

### `put`

Write a key→blob mapping. Appends a TYPE-PUT record and commits a new superblock. If the key already exists the old entry is superseded in the in-RAM index (the old log record is orphaned until a future log compaction, which is not yet implemented).

- **Request:** `(put storage name key data reply)`
  - `storage` — corestorage block I/O handle
  - `name` — volume/device name string
  - `key` — string key (truncated to 64 characters)
  - `data` — `bytes` object containing the blob to store
  - `reply` — context to reply to
- **Reply:** `(complete ok)`, `(complete full)`, `(complete io-error)`, or `(complete no-volume)`

```scheme
(send cardfs-ctx (list 'put storage "ahci0" "mykey" my-bytes (self)))
;; recv -> ('complete 'ok)
;;      or ('complete 'full)       ; log exhausted — no GC implemented yet
;;      or ('complete 'io-error)
;;      or ('complete 'no-volume)  ; device not mounted
```

### `get`

Retrieve a stored blob. Reads all payload blocks and verifies the whole-payload CRC32; a CRC mismatch or a failed block read returns `'corrupt` rather than serving bad data.

- **Request:** `(get storage name key reply)`
  - `storage` — corestorage block I/O handle
  - `name` — volume/device name string
  - `key` — string key
  - `reply` — context to reply to
- **Reply:** `(got ok bytes)`, `(got miss #f)`, `(got corrupt #f)`, or `(got no-volume #f)`

```scheme
(send cardfs-ctx (list 'get storage "ahci0" "mykey" (self)))
;; recv -> ('got 'ok <bytes>)
;;      or ('got 'miss #f)
;;      or ('got 'corrupt #f)      ; CRC mismatch or block-read failure
;;      or ('got 'no-volume #f)
```

### `get-range`

Retrieve a byte slice `[off, off+len)` of a stored blob. Only the blocks covering the requested range are read. **The payload CRC32 is not verified** for ranged reads because the stored checksum covers the entire payload, not a slice; a failed block read still returns `'corrupt`. Use `get` when integrity verification is required.

- **Request:** `(get-range storage name key off len reply)`
  - `storage` — corestorage block I/O handle
  - `name` — volume/device name string
  - `key` — string key
  - `off` — byte offset (must be `>= 0` and `< object-size`)
  - `len` — byte count (must be `>= 0`; `off + len` must not exceed the object size)
  - `reply` — context to reply to
- **Reply:** `(got ok bytes)`, `(got miss #f)`, `(got range #f)` (bounds error), `(got corrupt #f)`, or `(got no-volume #f)`

```scheme
(send cardfs-ctx (list 'get-range storage "ahci0" "mykey" 0 256 (self)))
;; recv -> ('got 'ok <bytes>)       ; bytes of length `len`
;;      or ('got 'range #f)         ; off/len out of bounds
;;      or ('got 'miss #f)
;;      or ('got 'corrupt #f)
;;      or ('got 'no-volume #f)
```

### `delete`

Remove a key by appending a TYPE-TOMB (tombstone) record. The entry is removed from the in-RAM index; the old log record is orphaned (no GC).

- **Request:** `(delete storage name key reply)`
  - `storage` — corestorage block I/O handle
  - `name` — volume/device name string
  - `key` — string key
  - `reply` — context to reply to
- **Reply:** `(complete ok)`, `(complete miss)` (key not found), `(complete io-error)`, or `(complete no-volume)`

```scheme
(send cardfs-ctx (list 'delete storage "ahci0" "mykey" (self)))
;; recv -> ('complete 'ok)
;;      or ('complete 'miss)
;;      or ('complete 'io-error)
;;      or ('complete 'no-volume)
```

### `stat`

Return metadata for a stored object without reading its payload.

- **Request:** `(stat storage name key reply)`
  - `storage` — corestorage block I/O handle (present in the message but not used by `do-stat`; the stat is served from the in-RAM index)
  - `name` — volume/device name string
  - `key` — string key
  - `reply` — context to reply to
- **Reply:** `(stat (size id))` on success, `(stat #f)` if the key is not found, or `(stat no-volume)` if the device is not mounted
  - `size` — payload size in bytes
  - `id` — numeric object ID assigned at put time

```scheme
(send cardfs-ctx (list 'stat storage "ahci0" "mykey" (self)))
;; recv -> ('stat (256 3))   ; size=256 bytes, id=3
;;      or ('stat #f)        ; key not found
;;      or ('stat 'no-volume)
```

### `keys`

Return a list of all live keys on a mounted volume (read from the in-RAM index).

- **Request:** `(keys storage name reply)`
  - `storage` — corestorage block I/O handle (present but not used; served from in-RAM index)
  - `name` — volume/device name string
  - `reply` — context to reply to
- **Reply:** `(keys (key ...))` — a list of key strings, or `(keys no-volume)` if the device is not mounted

```scheme
(send cardfs-ctx (list 'keys storage "ahci0" (self)))
;; recv -> ('keys ("mykey" "other"))
;;      or ('keys 'no-volume)
```

## Exported functions

### `(start-cardfs storage)`

The sole exported function. Spawns the cardfs provider context (as `spawn-restricted` with no capabilities), registers it with corestorage, and returns the provider context handle.

- `storage` — context handle for the corestorage registry (returned by `start-storage-service`)
- Returns the newly spawned provider context handle

## Notes / gotchas

**Single-context serialisation via `stash`.** cardfs is one context. Block I/O is performed by sending a `read` or `write` message to the storage handle and waiting for a `(complete ...)` reply (`io-recv`). During that wait any other message that arrives is pushed onto the `stash` list. When the I/O completes, the main dispatch loop drains `stash` before calling `recv` again, so overlapping requests are serialised in arrival order. There is no concurrency; a slow device stalls all other callers for the duration.

**No implicit format.** `probe` is read-only. A device with no valid CARDLOG1 superblock is silently declined. A `format` message must be sent explicitly (typically by a setup tool or first-use path) before `put`/`get`/`delete` can operate on a blank device.

**Log-only growth; no GC.** The log is append-only. Overwriting a key orphans the previous log record; deleting a key appends a tombstone but frees no space. When the log fills the device, `put` and `delete` return `'full`/`'io-error`. Log compaction is not implemented; this is a known limitation documented in the source.

**Ranged reads are not integrity-verified.** `get-range` reads only the blocks needed for the requested slice and cannot check them against the whole-payload CRC32 stored in the index. A block-read failure still returns `'corrupt`, but a bit-flip in the returned slice goes undetected. Use `get` when the integrity guarantee matters.

**Key truncation.** Keys longer than 64 bytes are silently truncated to their first 64 characters at both write time and read time (including log replay). Two keys that share a 64-character prefix are the same key from cardfs's perspective.

**No cache-flush primitive.** Crash safety depends on the storage path completing each block write before its `(complete ...)` reply (i.e. the underlying driver does not reorder writes across its ack). A device that reorders could, in the worst case, see the new superblock before the log record it points to, producing a torn read on remount. The CRC check on the record header would catch this and stop replay at the torn record, so corrupt data is never served, but the write may be lost. See `notes/servers/CoreStorage` for discussion.

**Multi-volume support.** cardfs maintains a `vols` hash-table of `name → volume-state`. Multiple block devices (e.g. both an AHCI disk and a USB mass-storage device) can be mounted simultaneously under different names.
