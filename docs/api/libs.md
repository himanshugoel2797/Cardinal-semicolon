# Static libraries (`libs/`)

API reference for the static libraries linked into Cardinal; kernel modules,
servers, and drivers. These are the building blocks below the module ABI:
crypto (the signing/verify boundary), the key/value store, the header-only PCI
helpers every driver uses, the syscall wrappers, and the CELF module-header
format. Third-party and very large internal surfaces (miniz, the Lisp VM C API,
PNG, the UBSan handlers) are documented as single `overview` entries.

---

## `sha256_init`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/sha256.c`
- **hash:** 36ea3fc1ff98df6e

Initialize a `SHA256_CTX` to begin a new SHA-256 digest.

**Parameters**

- `ctx` — the context to reset; must outlive the `update`/`final` sequence.

Sets the eight standard SHA-256 initial state words and zeros the length
counters. Call this once before any `sha256_update`. Defined in
`libs/crypto/sha256.h` with `SHA256_BLOCK_SIZE` (32) as the digest length.

---

## `sha256_update`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/sha256.c`
- **hash:** 4f1518918f4b875d

Feed `len` bytes of `data` into an in-progress SHA-256 digest.

**Parameters**

- `ctx` — an initialized `SHA256_CTX`.
- `data` — input bytes (treated as opaque `BYTE`/`unsigned char`).
- `len` — number of bytes.

Buffers into the internal 64-byte block and runs the compression function once
per full block, accumulating the total bit length. May be called any number of
times to stream a message; the result is identical to one call over the
concatenation.

---

## `sha256_final`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/sha256.c`
- **hash:** 5f844b81f2511357

Pad the message, finish the digest, and write the 32-byte hash to `hash`.

**Parameters**

- `ctx` — the context fed by `sha256_update`.
- `hash` — output buffer of at least `SHA256_BLOCK_SIZE` (32) bytes.

Applies standard SHA-256 padding (the `0x80` marker, zero fill, and the
big-endian bit length), runs the final block(s), and serializes the eight state
words big-endian into `hash`. The context is left in a finalized state; reuse
requires another `sha256_init`.

---

## `hmac_init`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/hmac.c`
- **hash:** fe80bed241fe1d9b

Begin an HMAC-SHA256 over a fixed 32-byte key, priming the inner hash.

**Parameters**

- `ctx` — an `hmac_ctx` (holds the precomputed outer key pad plus the running
  inner `SHA256_CTX`).
- `key` — pointer to **exactly 32 bytes** (256 bits) of key material.

Computes `H(key)` to derive the inner (`0x36`-XORed) and outer (`0x5c`-XORed)
key pads per RFC 2104, stores the outer pad in `ctx`, and seeds the inner SHA-256
with the inner pad. Returns 0. This is the primitive behind CELF module
verification (`VerifyModule`) and the `KMOD_HMAC_Key.txt` / `SERV_HMAC_Key.txt`
signing keys; the key length is not the block size but the digest size, so pass
a 32-byte key.

---

## `hmac_update`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/hmac.c`
- **hash:** 578433b4715e3d75

Feed `message_len` bytes of `message` into the in-progress HMAC.

**Parameters**

- `ctx` — an `hmac_ctx` from `hmac_init`.
- `message` — input bytes.
- `message_len` — number of bytes.

Forwards directly to `sha256_update` on the inner hash; may be called repeatedly
to stream the authenticated data. Returns 0.

---

## `hmac_final`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/hmac.c`
- **hash:** eeab0fc0c6698b76

Finish the HMAC and write the 32-byte tag to `result`.

**Parameters**

- `ctx` — the `hmac_ctx` fed by `hmac_update`.
- `result` — output buffer of at least 32 bytes.

Finalizes the inner hash, then computes `H(o_key_pad || inner_digest)` to
produce the final tag — i.e. `HMAC(K,m) = H((K^opad) || H((K^ipad) || m))`.
Returns 0.

---

## `AES_ECB_encrypt`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/aes.c`
- **hash:** f6900857b6c5086a

Encrypt a buffer with AES-128 in ECB mode, 16 bytes at a time.

**Parameters**

- `input` — plaintext.
- `key` — 16-byte AES-128 key.
- `output` — ciphertext buffer (same length as input).
- `length` — byte length; processed in 16-byte blocks.

Tiny-AES-style implementation (`libs/crypto/aes.h` selects `AES128` with both
`ECB` and `CBC` modes enabled by default). ECB has no IV and no chaining; it is
provided for completeness and is not used for the module-signing boundary (that
is HMAC-SHA256). Pair with `AES_ECB_decrypt`.

---

## `AES_ECB_decrypt`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/aes.c`
- **hash:** f238b84af1125806

Decrypt an AES-128 ECB buffer produced by `AES_ECB_encrypt`.

**Parameters** mirror `AES_ECB_encrypt`: `input` ciphertext, 16-byte `key`,
`output` plaintext, `length` in bytes (16-byte blocks).

---

## `AES_CBC_encrypt_buffer`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/aes.c`
- **hash:** 3ff7b164e9f9331e

Encrypt a buffer with AES-128 in CBC mode using an initialization vector.

**Parameters**

- `output` — ciphertext buffer.
- `input` — plaintext.
- `length` — byte length (16-byte blocks).
- `key` — 16-byte AES-128 key.
- `iv` — 16-byte initialization vector.

CBC chains each block with the previous ciphertext (XOR before encrypt), so
identical plaintext blocks differ in the output. Pair with
`AES_CBC_decrypt_buffer` using the same key and IV.

---

## `AES_CBC_decrypt_buffer`

- **kind:** function
- **lang:** c
- **source:** `libs/crypto/aes.c`
- **hash:** 3127bf98982748cc

Decrypt an AES-128 CBC buffer produced by `AES_CBC_encrypt_buffer`.

**Parameters** mirror the encrypt variant: `output` plaintext, `input`
ciphertext, `length`, 16-byte `key`, 16-byte `iv`.

---

## `kvs_t`

- **kind:** struct
- **lang:** c
- **source:** `libs/kvs/kvs.h`
- **hash:** 3054b2a14417c53d

A single entry in the key/value store: a fixed-size key, a tagged value, and a
`next` link forming a singly-linked sibling list.

Each `kvs_t` holds a `char key[key_len]` (`key_len` is 228), a `key_hash`, a
`val_type` (one of the `kvs_val_type` tags: `none`/`sint`/`uint`/`str`/`ptr`/
`child`/`bool`), an `owner_locked` flag, a union of the possible values
(including `struct kvs *child` for nesting a sub-store), and a `next` pointer.
The store is therefore a linked list of entries, each of which may point at a
child list — a hierarchical key/value tree. This is the data structure behind
the `SysReg` registry. Operate on it only through the `kvs_*` API below.

---

## `kvs_create`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** ecf10249458a8439

Allocate and zero-initialize a new empty key/value store root.

**Parameters**

- `r` — out-param; receives the newly allocated `kvs_t *` root.

Returns `kvs_ok` (0) on success or `kvs_error_outofmemory` if allocation fails.
The returned root is an empty list head you then populate with `kvs_add_*`.

---

## `kvs_islocked`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 92d2512c0e20610f

Report whether an entry is currently owner-locked.

**Parameters**

- `r` — the entry.
- `status` — out-param set to the entry's `owner_locked` flag.

Returns `kvs_ok`. Use with `kvs_lockentry`/`kvs_unlockentry` to coordinate
exclusive ownership of an entry.

---

## `kvs_lockentry`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** f3c70a05d9dc63d4

Mark an entry owner-locked, claiming exclusive ownership.

**Parameters**

- `r` — the entry to lock.

Returns `kvs_ok`, or `kvs_error_invalidargs` if already locked. This is an
advisory ownership flag on the entry (`owner_locked`), not a spinlock; callers
that need mutual exclusion against concurrent traversal also hold an external
lock (see `kvs_walk_path`).

---

## `kvs_unlockentry`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** a28c66c4c0f672f1

Clear an entry's owner-locked flag.

**Parameters**

- `r` — the entry to unlock.

Returns `kvs_ok`. Releases the ownership claimed by `kvs_lockentry`.

---

## `kvs_add_sint`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 8255be24096e2dfc

Add a new signed-integer (`int64_t`) entry under `key`.

**Parameters**

- `r` — the parent store.
- `key` — the new key (must not already exist).
- `sval` — the signed value.

Returns `kvs_ok`, `kvs_error_exists` if the key is already present, or
`kvs_error_outofmemory`. The value type is recorded as `kvs_val_sint`.

---

## `kvs_add_uint`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 293b35c29739c977

Add a new unsigned-integer (`uint64_t`) entry under `key`.

**Parameters**: `r` parent store, `key` (must not exist), `uval` value. Returns
`kvs_ok` / `kvs_error_exists` / `kvs_error_outofmemory`. Type recorded as
`kvs_val_uint`.

---

## `kvs_add_str`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** ec4056947e38e27e

Add a new string entry under `key`.

**Parameters**: `r` parent store, `key` (must not exist), `strval` the string
pointer to store. Returns `kvs_ok` / `kvs_error_exists` / `kvs_error_outofmemory`.
Type recorded as `kvs_val_str`. The store keeps the supplied pointer; ownership
of the backing string is the caller's concern.

---

## `kvs_add_ptr`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 9b12220e8ca68a8e

Add a new opaque-pointer entry under `key`.

**Parameters**: `r` parent store, `key` (must not exist), `ptrval` value. Returns
`kvs_ok` / `kvs_error_exists` / `kvs_error_outofmemory`. Type recorded as
`kvs_val_ptr`.

---

## `kvs_add_bool`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 1dacda1111b8bd10

Add a new boolean entry under `key`.

**Parameters**: `r` parent store, `key` (must not exist), `sval` value. Returns
`kvs_ok` / `kvs_error_exists` / `kvs_error_outofmemory`. Type recorded as
`kvs_val_bool`.

---

## `kvs_add_child`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 4e8bcb82d9f31433

Add a nested child store under `key`, forming a hierarchical tree.

**Parameters**: `r` parent store, `key` (must not exist), `childval` an existing
`kvs_t *` (typically from `kvs_create`) to attach as the child list. Returns
`kvs_ok` / `kvs_error_exists` / `kvs_error_outofmemory`. Type recorded as
`kvs_val_child`; retrieve it with `kvs_get_child` or traverse with
`kvs_walk_path`.

---

## `kvs_find`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 57f3e795ab6d4ca8

Look up an entry by key within one store level (no path traversal).

**Parameters**

- `r` — the store to search.
- `key` — the key to match (by hash then string compare).
- `res` — out-param receiving the matching `kvs_t *`; may be `NULL` to test for
  existence only.

Returns `kvs_ok` if found, `kvs_error_notfound` otherwise. Searches only the
immediate sibling list of `r`; to descend a `/`-separated path use
`kvs_walk_path`.

---

## `kvs_next`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 78cb963cddce7283

Advance an entry cursor to the next sibling in the list.

**Parameters**

- `r` — in/out: `*r` is updated to `(*r)->next`.

Returns `kvs_ok`, or `kvs_error_notfound` when the end of the list is reached.
Iterate a store level by walking from a child/root pointer with repeated
`kvs_next`.

---

## `kvs_get_key`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 295351cdb86d2e39

Copy an entry's key string into the caller's buffer.

**Parameters**: `r` the entry, `key` destination buffer (at least `key_len`).
Returns `kvs_ok`.

---

## `kvs_get_sint` / `kvs_get_uint` / `kvs_get_str` / `kvs_get_ptr` / `kvs_get_bool` / `kvs_get_child`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** pending

Read an entry's value as a specific type into the caller's out-param.

Each typed getter takes the entry `r` and a typed out-pointer
(`int64_t*` / `uint64_t*` / `char**` / `uintptr_t*` / `bool*` / `kvs_t**`
respectively) and returns `kvs_ok` on success or `kvs_error_invalidargs` /
`kvs_error_notfound` if the entry's `val_type` does not match the requested type.
Use `kvs_get_type` first if the stored type is unknown.

---

## `kvs_set_sint` / `kvs_set_uint` / `kvs_set_str` / `kvs_set_ptr` / `kvs_set_bool`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** pending

Overwrite an existing entry's value in place with a new value of the same type.

Each typed setter takes the entry `r` and the new value, returning `kvs_ok` or a
`kvs_error_*` code on type mismatch. Unlike `kvs_add_*` these mutate an entry you
already located with `kvs_find` / `kvs_walk_path` rather than inserting a new key.
`kvs_set_key` (rename) is also provided.

---

## `kvs_get_type`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 66ca7b81bb462d91

Report the stored value type of an entry.

**Parameters**: `idx` the entry, `val_type` out-param receiving a `kvs_val_type`
(`uninit`/`none`/`sint`/`uint`/`str`/`ptr`/`child`/`bool`). Returns `kvs_ok`.
Call before a typed `kvs_get_*` when the entry's type is not known statically.

---

## `kvs_remove`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** 68b64157478f8b50

Unlink and free a single entry `idx` from store `r`.

**Parameters**: `r` the parent store, `idx` the entry to remove. Returns `kvs_ok`
or `kvs_error_notfound` if `idx` is not in `r`'s list. Frees the entry node; any
string/pointer payload it referenced is the caller's responsibility.

---

## `kvs_delete`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** ac0b5ac5c10d9bdd

Free an entire store, recursively deleting child stores.

**Parameters**: `r` the store root to destroy. Returns `kvs_ok`. Walks the
sibling list freeing every node and descending into `kvs_val_child` entries.

---

## `kvs_walk_path`

- **kind:** function
- **lang:** c
- **source:** `libs/kvs/kvs.c`
- **hash:** d9f8352ca13153d7

Resolve a `/`-separated path from a root, descending through child stores under
a caller-supplied lock.

**Parameters**

- `root` — the starting store.
- `lock` — pointer to a `local_spinlock` int acquired/released around the walk.
- `max_keylen` — maximum length allowed per path component.
- `path` — the `/`-separated path; an empty path resolves to `root`.
- `out` — out-param receiving the final component's `kvs_t *`.

Returns `CS_OK` on success, or `CS_DNE` if any component is missing or exceeds
`max_keylen`. On an empty path `*out` is set to `root` without touching the lock.
This is the traversal primitive the `SysReg` registry layers its
`registry_readkey_*` / `registry_addkey_*` helpers on top of.

---

## `pci_config_t`

- **kind:** struct
- **lang:** c
- **source:** `libs/pci/pci.h`
- **hash:** 06703d86002e201d

A `PACKED` overlay of the type-0 PCI configuration-space header, mapped onto a
device's ECAM region.

Fields mirror the PCI spec: `vendorID`/`deviceID`, the bit-field
`pci_command_reg_t command` (with `mem_space`, `busmaster`, `int_disable`
bits used by the allocation/IRQ helpers), `status`, `revisionID`/`classCode`,
the six `bar[6]` slots, `subsystem`/`subsystem_vendor`, `expansion_rom`, and
`capabilitiesPtr` (the head of the capability linked list). A PCI driver's
`module_init(void *ecam)` maps its ECAM and casts it to `pci_config_t *`; all the
header-only helpers below take this pointer. Companion capability structs
(`pci_cap_header_t`, `pci_msi_32_t`/`pci_msi_64_t`, `pci_msix_t`) and the
`pci_cap_ids_t` enum are defined alongside it.

---

## `pci_read_bar`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci.h`
- **hash:** 6f97c42b79c52919

Assemble the next MMIO base address from a device's BAR array, advancing the
caller's scan index past the BAR(s) it consumed.

**Parameters**

- `device` — the mapped `pci_config_t`.
- `idx` — in/out scan cursor; advanced by 2 for a 64-bit BAR, 1 for a 32-bit
  BAR, and set to 6 when no MMIO BAR remains.

Returns the BAR's base physical address, or 0 (and `*idx == 6`) if none is left.
`static inline` in `pci.h`. Skips I/O-space BARs and empty (zero-address) BARs,
so it finds the real MMIO BAR even when it is a later slot (e.g. AHCI's ABAR is
BAR5). Use this to iterate multiple MMIO BARs; for the common single-BAR case
use `pci_first_mmio_bar`.

---

## `pci_first_mmio_bar`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci.h`
- **hash:** 4e865c158dad9e72

Return the first MMIO BAR base of a device — the usual path for a
firmware-configured device.

**Parameters**: `device` the mapped `pci_config_t`. Returns the base address, or
0 if the device exposes no MMIO BAR. `static inline` in `pci.h`; a thin wrapper
over `pci_read_bar` starting the scan at index 0. This works when firmware
already assigned the device's BARs (the common case). A device firmware never
used at boot can have *unassigned* BARs and a *closed* bridge window — then call
`pci_assign_bars` (`pci_alloc.h`) instead.

---

## `pci_getmsiinfo`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci.h`
- **hash:** 4fe40562f2712e1a

Walk a device's capability list to detect MSI/MSI-X and report the vector count.

**Parameters**: `device` the mapped config space, `cnt` out-param set to the
number of supported vectors. Returns `1` if MSI-X is present, `0` if (only) MSI
is present, or `-1` if neither. `static inline` in `pci.h`. Used by the IRQ
helpers to decide which capability to program.

---

## `pci_setmsiinfo`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci.h`
- **hash:** 0c737b4f8a783ff1

Program a device's MSI or MSI-X capability with a message address/data and
enable it.

**Parameters**

- `device` — the mapped config space.
- `msix` — `1` to program the MSI-X capability, `0` for MSI.
- `msi_addr` / `msi_msg` — arrays of message address(es) and data; a single pair
  when `cnt == 1`, otherwise one per vector.
- `cnt` — vector count.

Returns 0, or `-1` if an MSI-X table-size mismatch is detected. `static inline`
in `pci.h`. For MSI it handles the 32- vs 64-bit capability layout; for MSI-X it
maps the table BAR (via `vmem_phystovirt`, uncached) and writes each table entry.
Enabling the capability is the last thing it does — see `pci_setup_msi_handler`
for why the handler must be registered *before* this runs. Most drivers call the
`pci_irq.h` wrappers rather than this directly.

---

## `pci_setup_msi`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_irq.h`
- **hash:** d87116d0432394d9

Allocate one interrupt vector and program the device's MSI(-X) capability to
deliver to it on CPU 0.

**Parameters**: `device` the mapped config space, `flags` `interrupt_flags_t`
for the vector allocation. Returns the allocated vector (>= 0), or `-1` if the
device has no MSI capability. `static inline` in `pci_irq.h`; bridges `pci.h`'s
config-space helpers and `SysInterrupts`. Prefer `pci_setup_msi_handler`, which
closes the lost-first-interrupt race by registering the handler before enabling
the capability.

---

## `pci_setup_msi_handler`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_irq.h`
- **hash:** 72e29586ff43a52c

Allocate an interrupt vector, register `handler` for it, and *then* enable the
device's MSI(-X) capability — the race-free MSI setup path.

**Parameters**

- `device` — the mapped config space.
- `flags` — `interrupt_flags_t` for the allocation.
- `handler` — the `InterruptHandler` to install.

Returns the allocated vector (>= 0), or `-1` if the device has no MSI capability.
`static inline` in `pci_irq.h`. **Ordering matters**: `pci_setmsiinfo` turns the
capability on, after which the device may immediately emit a message for an
already-pending cause. If the handler is not registered yet, that first message
hits an unhandled vector and is dropped, and an edge-triggered MSI(-X) never
re-fires — wedging interrupts for the whole session. Registering the handler
first closes that window. The caller must still enable its device's *own*
interrupt mask (e.g. a NIC's IMR) only **after** this returns. Vector 0 is
reserved, so this does not fall back to it on allocation failure.

---

## `pci_bar_getsize`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_alloc.h`
- **hash:** 01d449c8333a29b5

Size a BAR slot in place by the write-ones / read-mask / restore trick, 64-bit
aware.

**Parameters**: `bars` the device's `bar[]` array (volatile), `i` the slot
index. Returns the BAR's size in bytes, or 0 if the slot is unimplemented or an
I/O BAR. `static inline` in `pci_alloc.h`. The caller must have the device's
memory decode disabled (writing all-ones to a live BAR would move its window).
Used internally by `pci_mmio_used_top` and `pci_assign_bars`.

---

## `pci_mmio_used_top`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_alloc.h`
- **hash:** 4047606da28b03ae

Find the highest end address of any assigned memory BAR below `ceiling` by
scanning every enumerated PCI device.

**Parameters**: `ceiling` the upper bound (typically the host-bridge aperture
top). Returns the top of the used MMIO region, or 0 if the PCI registry is
unavailable. `static inline` in `pci_alloc.h`; requires `SysReg` to enumerate
devices. Runs during driver load (before APs/poll tasks exist), so briefly
toggling a neighbour's memory decode to size its BARs is safe. Used by
`pci_assign_bars` to place a new BAR just above the firmware-assigned region.

---

## `pci_bridge_open_window`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_alloc.h`
- **hash:** 3e6817654d28b4de

Open the memory-forwarding window covering `[base, base+size)` on every
PCI-to-PCI bridge between a device and the root bus.

**Parameters**: `dev_ecam` the device's ECAM physical address (its bus is
decoded from it), `base`/`size` the MMIO range to forward. Returns `true` if the
device is on the root bus (nothing to do) or at least one forwarding bridge was
programmed. `static inline` in `pci_alloc.h`. Handles PCIe root ports and nested
bridges, rounds the window to 1 MiB granularity, *extends* an already-open window
rather than clobbering it (so siblings behind the same bridge keep working), and
enables memory decode + bus-master on each touched bridge — bus-master so the
device's upstream MSI/MSI-X writes are forwarded too.

---

## `pci_assign_bars`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_alloc.h`
- **hash:** 92cb2b8dac9f572c

Assign MMIO addresses to a device's unassigned BARs and open the bridge path so
they are reachable — the recovery path for a device firmware never configured.

**Parameters**

- `device` — the mapped `pci_config_t`.
- `ecam_phys` — the device's ECAM physical address (its bus and the placement
  ceiling are derived from it).

Returns the base of the first non-prefetchable (register) memory BAR, or 0 on
failure. `static inline` in `pci_alloc.h`; requires `SysReg` and
`SysVirtualMemory`. Disables memory decode, packs every unassigned size-aligned
memory BAR into a free region just above the existing assignments
(`pci_mmio_used_top`), opens the forwarding window on every bridge to the root
bus (`pci_bridge_open_window`), then re-enables the device's memory decode +
bus-master. **On any failure it leaves memory decode disabled** so the device
cannot decode MMIO at wrong addresses against partially-assigned BARs. Use this
instead of `pci_first_mmio_bar` for a device (e.g. an onboard NIC behind a PCIe
root port) that came up with unassigned BARs and a closed bridge window.

---

## `pci_msix_debug_dump`

- **kind:** function
- **lang:** c
- **source:** `libs/pci/pci_debug.h`
- **hash:** 31fb581a2b71531f

Dump a device's MSI-X capability and table entry 0 over `DEBUG_PRINT` to
diagnose non-delivering MSI-X.

**Parameters**: `device` the mapped config space. `static inline` in
`pci_debug.h` (kept out of `pci.h` so normal users don't pull in the debug
dependency). Prints the enable / function-mask bits, the table BIR and address,
and the message address/data + per-vector control read back from the table BAR.
Reading the output: `addrLo=0x0` means the table write never reached the BAR
(wrong BIR, or an unreachable/closed bridge window over it); `vctl` bit 0 set
means the vector is masked; `addrLo=0xFEExxxxx` with `en=1` and `vctl` bit 0
clear means the table is correct and the fault is upstream message routing
(bridge bus-master or LAPIC). Prints nothing if the device has no MSI-X
capability.

---

## `cs_syscall0` … `cs_syscall6`

- **kind:** function
- **lang:** c
- **source:** `libs/syscalls/cs_syscall.h`
- **hash:** pending

The register-based syscall wrappers: invoke syscall `n` in syscall-set `s` with
0 to 6 arguments via the `syscallq` instruction.

Each `cs_syscallN(s, n, a1..aN)` is a `static __inline` function that loads the
syscall set into `r13` and the call number into `r12`, places arguments in the
System V argument registers (`rdi`, `rsi`, `rdx`, then `r10`, `r8`, `r9` for the
4th–6th — `r10` substitutes for `rcx`, which `syscallq` clobbers), executes
`syscallq`, and returns the result from `rax`. All clobber `rcx`, `r11`, and
`memory`. These are the raw entry points; the typed helpers below wrap the
default set's well-known calls. `syscall_set` 0 (`CS_SYSCALLSET_DEFAULT`) is
always available; other sets are obtained via `cs_openspecialset`.

---

## `cs_nanosleep`

- **kind:** function
- **lang:** c
- **source:** `libs/syscalls/cs_syscall.h`
- **hash:** 42793c055f5ea8c0

Sleep the current task for `ns` nanoseconds (default-set syscall
`CS_SYSCALL_NANOSLEEP`).

**Parameters**: `ns` duration in nanoseconds. Returns a `cs_error`. `static
__inline` wrapper over `cs_syscall1(CS_SYSCALLSET_DEFAULT,
CS_SYSCALL_NANOSLEEP, ns)`.

---

## `cs_endtask`

- **kind:** function
- **lang:** c
- **source:** `libs/syscalls/cs_syscall.h`
- **hash:** 3cd099aae4a36bf8

Terminate the current task (default-set syscall `CS_SYSCALL_ENDTASK`).

Takes no arguments; returns a `cs_error` (does not return on success). `static
__inline` wrapper over `cs_syscall0`.

---

## `cs_openspecialset`

- **kind:** function
- **lang:** c
- **source:** `libs/syscalls/cs_syscall.h`
- **hash:** 52ccc1551e1cbc85

Open a special syscall set by id and obtain its call index for use as the `s`
argument to `cs_syscallN`.

**Parameters**: `set_id` the requested set, `call_idx` out-param receiving the
opened set's index. Returns a `cs_error`. `static __inline` wrapper over
`cs_syscall2(CS_SYSCALLSET_DEFAULT, CS_SYSCALL_OPENSPECIALSET, ...)`. This is how
a task reaches syscall sets beyond `CS_SYSCALLSET_DEFAULT`.

---

## `cs_error`

- **kind:** constant
- **lang:** c
- **source:** `common/inc/cardinal/cs_error.h`
- **hash:** 1ff7dba6330a04a5

The unified error type and codes for all `Sys*` module APIs (re-exported via
`libs/syscalls/error.h`).

`cs_error` is `typedef int`. `CS_OK` is 0; **every failure is negative**, so
`if (err < 0)` reliably detects any error. The codes: `CS_OK` (0),
`CS_UNKN` (-1, unspecified), `CS_OUTOFMEM` (-2), `CS_INVALIDARG` (-3),
`CS_DNE` (-4, does not exist / not found), `CS_EXISTS` (-5), `CS_FAILURE` (-6),
`CS_TYPEMISMATCH` (-7, value present but wrong type), `CS_ALREADYMAPPED` (-8),
`CS_NOMAPPING` (-9), and `CS_CONTINUE` (-10, internal "keep walking", not a
terminal error). These live in `common/inc/cardinal/cs_error.h` so every module
gets them without a `libs/syscalls` include; `libs/syscalls/error.h` just
`#include`s that header for existing includers of `<error.h>` / `cs_syscall.h`.

---

## `ModuleHeader`

- **kind:** struct
- **lang:** c
- **source:** `libs/module_lib/module_def.h`
- **hash:** fe4894853b250003

The CELF header prepended to a relocatable ELF to make a signed, loadable
Cardinal; module — the security boundary the kernel verifies before loading.

A `ModuleHeader` (`libs/module_lib/module_def.h`) carries the `'CELF'` magic
(`MODULE_HEADER_MAGIC`), the `module_name` and two device-name strings, major/
minor version, the payload's `elf_len` and `uncompressed_len`, a `module_nid`
numeric id, a truncated `key_hash` (32 bytes) identifying the signing key, the
32-byte HMAC-SHA256 `hash` over the header (with the hash field zeroed) plus
payload, and the flexible-array `data[]` holding the (optionally miniz-compressed)
ELF. The kernel runs `VerifyModule` on this before loading. Companion types
`Import` / `ImportFunction` / `Export` describe the inter-module symbol fixups
the loader resolves. Two signing keys exist: `KMOD_HMAC_Key.txt` for `modules/`
(`Sys*`) and `SERV_HMAC_Key.txt` for `servers/` and `drivers/`.

---

## `VerifyModule`

- **kind:** function
- **lang:** c
- **source:** `libs/module_lib/module.c`
- **hash:** 577df2ea311a902d

Verify a CELF module's magic and HMAC-SHA256 signature against a key — the gate
the kernel runs before loading any module.

**Parameters**

- `hdr` — the `ModuleHeader` to verify.
- `key` — the 32-byte HMAC key (`KMOD_*` or `SERV_*` per module class).

Returns 0 on a valid module, non-zero on failure. Copies the header, zeroes the
`hash` field, checks the `'CELF'` magic, recomputes `HMAC(key, header || payload)`
via `hmac_init`/`hmac_update`/`hmac_final`, and compares it to the stored hash.
A mismatch — tampered payload, wrong key, or corrupted header — fails
verification. This is the trust boundary: only modules signed with the matching
key load.

---

## `BuildModuleHeader`

- **kind:** function
- **lang:** c
- **source:** `libs/module_lib/module.c`
- **hash:** 4cf4ef1da566140a

Populate and sign a `ModuleHeader` over an ELF payload — the host-side counterpart
to `VerifyModule`, used by the `sign_exec` tool.

**Parameters**

- `hdr` — the header to fill (sized to hold the payload after it).
- `module_name`, `dev_name`, `dev_name2` — name strings.
- `min_ver`, `maj_ver` — version strings (parsed as hex into the version fields).
- `key` — the HMAC signing key.
- `elf` — the (optionally compressed) ELF payload.
- `elf_len`, `uncompressed_len` — payload length and its uncompressed size.

Returns 0 on success. Writes the magic, names, versions, lengths, and key hash,
copies the payload into `data[]`, then computes the HMAC-SHA256 over the header
(hash field zeroed) plus payload and stores it in `hash`. The resulting `.celf`
is what `VerifyModule` later checks. Built by the **host** tool in
`utils/sign_exec/`, not by target modules.

---

## `libs/miniz`

- **kind:** overview
- **lang:** c
- **source:** `libs/miniz/miniz.h`
- **hash:** pending

Vendored third-party miniz 2.0.6 — a public-domain, single-file deflate/inflate
and zlib-subset library.

miniz implements RFC 1950 (zlib) and RFC 1951 (deflate) with a fairly large
drop-in zlib-compatible surface (`compress`/`uncompress`, `deflate*`/`inflate*`,
`z_stream`, CRC-32/Adler-32) plus the low-level allocation-free `tdefl`
(compress) and `tinfl` (decompress) APIs, and optional ZIP archive and PNG-write
helpers (disabled where not needed). In Cardinal; it provides the (de)compression
used for CELF module payloads (`uncompressed_len` in `ModuleHeader`) and the PNG
decoder. This is unmodified upstream code; do not document its functions
per-symbol — see the header `libs/miniz/miniz.h` and the upstream project
(<https://github.com/richgel999/miniz>) for the full API. License: public-domain
"unlicense" (`libs/miniz/LICENSE`).

---

## `libs/lisp`

- **kind:** overview
- **lang:** c
- **source:** `libs/lisp/inc/lisp.h`
- **hash:** pending

The C API of the kernel-resident Scheme VM: the tagged value representation,
reader/printer, evaluator, and host-binding surface that the Lisp servers and
drivers run on.

`libs/lisp/inc/lisp.h` defines the core `lisp_value` (a one-word tagged value:
low-2-bit tag for heap pointer / 62-bit fixnum / immediate, with `#t`/`#f`/`()`/
char/eof/undef immediates), the heap-object headers, and the C entry points to
read, evaluate, and print Lisp, plus the substrate for exposing C primitives
(the `sys-*` driver/capability prims) to Lisp code. The dialect is Scheme-like
but immutable-by-default with persistent data structures (load-bearing for
persistence, checkpoints, and lock-free concurrency) — see
`notes/core/lisp-substrate.md`. This is a large internal surface documented as an
overview; the **Lisp *language* API** (the `sys-*` primitives, the server/driver
bindings, `man`/`apropos`) is documented separately under the `lisp-*.md` docs.
For the C VM internals see `libs/lisp/inc/lisp.h` and `libs/lisp/src/`.

---

## `libs/png`

- **kind:** overview
- **lang:** c
- **source:** `libs/png/png.h`
- **hash:** pending

A minimal PNG decoder: `DecodePNGtoRGBA` turns an 8-bit non-interlaced truecolor
(RGB) or truecolor+alpha (RGBA) PNG into a freshly `malloc`'d BGRA8888 buffer.

`void *DecodePNGtoRGBA(const void *src, int len, int *img_w, int *img_h, int
*img_p, int *res_len)` returns the pixel buffer (caller `free()`s it) or `NULL`
on error or unsupported format; the optional out-params receive width, height,
pitch (`width*4` bytes/row), and total byte length. The output byte order is
B, G, R, A — read as a little-endian `uint32` that is `0xAARRGGBB`, the
XRGB8888 format used by the display planes / linear framebuffer. The decoder uses
`libs/miniz` for the zlib-compressed image data. Only the common 8-bit
non-interlaced RGB/RGBA subset is supported; other PNG variants return `NULL`.
Single public entry point — documented as an overview.

---

## `libs/ubsan_handlers`

- **kind:** overview
- **lang:** c
- **source:** `libs/ubsan_handlers/ubsan.c`
- **hash:** pending

Freestanding Undefined-Behavior Sanitizer runtime: the `__ubsan_handle_*`
callbacks the compiler emits when modules are built with `-fsanitize=undefined`.

`libs/ubsan_handlers/ubsan.c` provides the handler functions (e.g. integer
overflow, shift-out-of-bounds, type/alignment mismatches, out-of-bounds index)
that Clang's UBSan instrumentation calls at runtime, together with the
`type_descriptor` / `source_location` / per-check `*_data` structs the compiler
passes them. They report the offending source location (filename / line /
column) over the kernel debug output rather than depending on a hosted libc. This
is a compiler-ABI-driven internal runtime, not a user-facing API; the handler
names and signatures are fixed by the Clang UBSan contract. Documented as an
overview — see `libs/ubsan_handlers/ubsan.c`.

---
