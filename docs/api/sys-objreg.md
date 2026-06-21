# SysObj & SysReg — object model and registry

These two kernel-privileged modules expose a single shared abstraction: an
in-memory, hierarchical key/value store (a "directory tree"), built on top of
the `libs/kvs` key/value-store primitive. **SysReg** is the system **registry** —
a hierarchical key/value store that other modules use heavily for hardware
topology and configuration (it pre-creates `HW/`, `HW/BOOTINFO`, `HW/PROC`,
`HW/PHYS_MEM`, `HW/VIRT_MEM`, `HW/CACHE`, `HW/CACHE/TLB` at init and populates
them with ACPI/PCI/bootinfo data). **SysObj** is a second, independent tree (the
"object model") with the same shape plus per-node **locking** and **typed
in-place writes**.

Both surfaces follow the same conventions:

- **Paths** are `/`-separated directory paths from the tree root; the root is the
  empty string `""`. A path identifies a *directory* (node); `keyname` /
  `dirname` is a single component under it. Depth is capped at
  `MAX_OBJ_DEPTH` / `MAX_REGISTRY_DEPTH` (20), key/name length at
  `MAX_OBJ_KEYLEN` / `MAX_REGISTRY_KEYLEN` (200), stored strings at
  `MAX_OBJ_STRLEN` / `MAX_REGISTRY_STRLEN` (4096).
- **Naming.** This is a key/value store, so accessors use `add`/`read`/`write`
  (`addkey`/`readkey`/`writekey`), deliberately distinct from the `get`/`set`
  verbs used elsewhere for scalar CPU/hardware state.
- **Types.** Every key is one of five value types — `uint` (`uint64_t`), `ptr`
  (`uintptr_t`), `int` (signed `int64_t`), `str` (C string), `bool` — plus the
  directory (child node) type. Reading or writing a key with the wrong-typed
  accessor returns `CS_TYPEMISMATCH`; the type is fixed at `addkey` time.
- **Return values** are `cs_error` (`common/inc/cardinal/cs_error.h`):
  `CS_OK` on success, `CS_INVALIDARG` for a NULL `path`/`keyname`,
  `CS_DNE` when a key/node does not exist, `CS_EXISTS` when creating a directory
  that is already present, `CS_TYPEMISMATCH` on a type-wrong access,
  `CS_FAILURE` for an underlying `kvs`/allocation failure.
- **Locking.** A single module-global spinlock (`kern_lock`) serializes every
  operation against its own tree; the two trees are independent. The lock is
  held only for the duration of each call. SysObj additionally exposes a
  *logical* per-node lock (`obj_lock`/`obj_unlock`) layered on top of this,
  used by `obj_removekey` to refuse deletion of a node a caller has claimed.

The `dir_t` type (`modules/inc/dir_t.h`) is an **opaque handle** (`void *`) to a
node, shared by both headers so they agree on the type. A `dir_t` obtained from
`*_getdirectory` points at a live `kvs` node; the `*_next` / `*_readlocal_*`
family walks and reads sibling nodes without re-resolving the path.

---

## `obj_createdirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** df035db4d289a45e

Create a new child directory `dirname` under the existing directory at `path` in
the object model.

**Parameters:** `path` — directory to create under (`""` for root); `dirname` —
the new child's name.
**Returns:** `CS_OK`; `CS_INVALIDARG` if either argument is NULL; the resolver's
error if `path` does not resolve; `CS_EXISTS` if a child `dirname` already
exists; `CS_FAILURE` if the underlying `kvs_create`/`kvs_add_child` fails. The
global lock is taken across the existence check and insert so the create is
atomic.

---

## `obj_addkey_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 7b01e83db3af19b8

Add a new unsigned-integer (`uint64_t`) key `keyname` with value `val` to the
directory at `path`.

**Parameters:** `path` — owning directory; `keyname` — new key; `val` — value.
**Returns:** `CS_OK`; `CS_INVALIDARG` for a NULL `path`/`keyname`; the resolver's
error if `path` is bad; `CS_FAILURE` if `kvs_add_uint` fails (e.g. the key
already exists or allocation failed). This is the canonical `addkey_*`: it
resolves the parent directory, then inserts a freshly-typed key under the global
lock. The type recorded here is what later `readkey`/`writekey` calls must
match. The four sibling variants below are identical apart from the value type.

---

## `obj_addkey_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 8b02f6a181ceaa27

Add a new pointer-typed (`uintptr_t`) key. See `obj_addkey_uint`; the value is
stored via `kvs_add_ptr` as a `ptr`-typed entry.

---

## `obj_addkey_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** bf7ac631e1639732

Add a new signed-integer (`int64_t`) key. See `obj_addkey_uint`; stored via
`kvs_add_sint` as an `int`-typed entry.

---

## `obj_addkey_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** a51a96c492a588fa

Add a new string key by copying `val` into a freshly-allocated, NUL-terminated
buffer owned by the store.

Unlike the scalar variants, this **deep-copies** the string: it allocates
`min(MAX_OBJ_STRLEN-1, strlen(val)) + 1` bytes, copies, and explicitly writes the
trailing `'\0'`. The explicit terminator is load-bearing — `common`'s `strncpy`
is non-standard (it stops at the source NUL and neither copies nor pads it), and
`obj_readkey_str` later does `strlen()` on the stored pointer, so an
unterminated copy would run off the allocation. Strings longer than
`MAX_OBJ_STRLEN-1` are silently truncated.
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL `path`/`keyname`; `CS_FAILURE` on
allocation failure or `kvs_add_str` failure. (Note: `val` is dereferenced via
`strlen` before the NULL checks on the path can short-circuit it — callers must
pass a non-NULL `val`.)

---

## `obj_addkey_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 59e9a1c307312af3

Add a new boolean key. See `obj_addkey_uint`; stored via `kvs_add_bool`.

---

## `obj_readkey_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 5fac852d890e6bd8

Read the `uint64_t` value of key `keyname` under `path` into `*val`.

**Parameters:** `path`, `keyname`; `val` — out-param (may be NULL to probe for
existence/type only).
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL `path`/`keyname`; `CS_DNE` if the
key is absent; `CS_TYPEMISMATCH` if it is not a `uint`; `CS_FAILURE` on an
underlying `kvs` error. This is the canonical `readkey_*`: under the lock it
finds the key, checks its type matches, then (if `val` is non-NULL) copies the
value out. The other scalar read variants are identical bar the type check and
getter.

---

## `obj_readkey_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 29e0c298103ec7f3

Read a `ptr`-typed key into `*val`. See `obj_readkey_uint`; requires
`kvs_val_ptr`.

---

## `obj_readkey_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** e14629896eda84c9

Read an `int`-typed (`int64_t`) key into `*val`. See `obj_readkey_uint`;
requires `kvs_val_sint`.

---

## `obj_readkey_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 9ade5153b2d698be

Read a string key into the caller's buffer `val` of capacity `*val_len`,
updating `*val_len` to the full source length.

**Parameters:** `path`, `keyname`; `val` — caller buffer (NULL probes type only);
`val_len` — in: buffer capacity, out: full source length (so a caller can detect
truncation and re-query with a bigger buffer).
**Returns:** `CS_OK`; `CS_INVALIDARG`/`CS_DNE`/`CS_TYPEMISMATCH` as for
`obj_readkey_uint` (requires `kvs_val_str`); `CS_FAILURE` on a `kvs` error. The
copy is truncation-safe: it copies at most `cap-1` bytes and **always** writes a
terminating `'\0'` itself, precisely because `common`'s `strncpy` does not
terminate. `*val_len` is set to the untruncated `strlen`, which may exceed the
buffer capacity.

---

## `obj_readkey_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** bcebdea88bc1160b

Read a `bool`-typed key into `*val`. See `obj_readkey_uint`; requires
`kvs_val_bool`.

---

## `obj_writekey_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 4b113c387c42f7c1

Overwrite the value of an existing `uint`-typed key `keyname` under `path` with
`val`.

**Parameters:** `path`, `keyname`, `val`.
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL `path`/`keyname`; `CS_DNE` if the
key does not exist (write does **not** create); `CS_TYPEMISMATCH` if the existing
key is not a `uint`; `CS_FAILURE` on a `kvs_set_*` error. This is the canonical
`writekey_*`: it finds the key, verifies the type matches, and sets the value
in place under the lock. The sibling variants differ only by type. SysReg has no
`writekey_*` family — once added, registry keys are read-only at this surface.

---

## `obj_writekey_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 74b1e3fabb54bfbd

Overwrite an existing `ptr`-typed key. See `obj_writekey_uint`; requires
`kvs_val_ptr`.

---

## `obj_writekey_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** d0428c2ca978e167

Overwrite an existing `int`-typed key. See `obj_writekey_uint`; requires
`kvs_val_sint`.

---

## `obj_writekey_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 22d9debdc6e6e296

Overwrite an existing `str`-typed key with the pointer `val`.

Unlike `obj_addkey_str`, this does **not** copy — `kvs_set_str` stores the bare
pointer (the `const` is cast away only because the store never writes through
it). The caller therefore must keep `val` alive for the lifetime of the key, and
the previously-stored string pointer is replaced, not freed. Also rejects a NULL
`val` with `CS_INVALIDARG` (the only `writekey` variant to NULL-check the value).
**Returns:** `CS_OK`; `CS_INVALIDARG`/`CS_DNE`/`CS_TYPEMISMATCH`/`CS_FAILURE` as
for `obj_writekey_uint` (requires `kvs_val_str`).

---

## `obj_writekey_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 2420a03655e25c6f

Overwrite an existing `bool`-typed key. See `obj_writekey_uint`; requires
`kvs_val_bool`.

---

## `obj_removekey`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** c2a32a9b4acbd3f9

Remove key (or child directory) `keyname` from the directory at `path`, unless it
is logically locked.

**Parameters:** `path`, `keyname`.
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL `path`/`keyname`; `CS_DNE` if the
key is absent (or its lock state cannot be queried). Note the locking semantics:
if the target node has been claimed via `obj_lock`, removal is **silently
skipped** but the call still returns `CS_OK` — callers cannot distinguish "removed"
from "left in place because locked" from the return value.

---

## `obj_removedirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 6ce3fdc97b0445eb

Remove child directory `dirname` from `path`; a thin alias for `obj_removekey`
(directories and keys share one removal path), so the lock-skip semantics apply.

---

## `obj_getdirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** dbc2ec285479d360

Resolve `path` to its node and return an opaque `dir_t` handle in `*dir` for
iteration/local reads.

**Returns:** the resolver's status (`CS_OK` or a walk error). The handle aliases
the live `kvs` node; use it with `obj_next` and the `obj_readlocal_*` family.

---

## `obj_next`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** d50e31d89c44e2a8

Advance the `dir_t` handle in `*dir` to the next sibling entry in the current
directory.

**Returns:** `CS_OK` if advanced; `CS_DNE` at end of the sibling list. Takes the
global lock for the step. Combined with `obj_getdirectory` +
`obj_readlocal_dir`, this enumerates a directory's children.

---

## `obj_readlocal_key`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** a31349b5b516d4e3

Copy the name of the entry the `dir_t` handle currently points at into the
caller-supplied `keyname` buffer.

**Returns:** `CS_OK`; `CS_DNE` on a `kvs` error. The buffer must be large enough
for the key name (up to `MAX_OBJ_KEYLEN`); the underlying `kvs_get_key` performs
the copy. The `readlocal_*` family reads the *current* node of a handle obtained
from `obj_getdirectory`/`obj_next`, without re-resolving a path.

---

## `obj_readlocal_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** badc6ffbfb6b778c

Read the current handle's value as `uint64_t` into `*val`. See
`obj_readlocal_key`; no explicit type check here — the underlying `kvs_get_uint`
fails (→ `CS_DNE`) if the node is not a `uint`.

---

## `obj_readlocal_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** e4b1d24e76a8f653

Read the current handle's value as a pointer into `*val`. See
`obj_readlocal_uint`; uses `kvs_get_ptr`.

---

## `obj_readlocal_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** c54c468d3d14ab3d

Read the current handle's value as `int64_t` into `*val`. See
`obj_readlocal_uint`; uses `kvs_get_sint`.

---

## `obj_readlocal_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 08c98c358f67ff5f

Return the current handle's stored string pointer in `*val` (no copy — the
caller borrows the store's buffer). See `obj_readlocal_uint`; uses
`kvs_get_str`.

---

## `obj_readlocal_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** a939f6e28b47b273

Read the current handle's value as `bool` into `*val`. See `obj_readlocal_uint`;
uses `kvs_get_bool`.

---

## `obj_readlocal_dir`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 3d1241845b861f77

Return a `dir_t` handle to the **first child** of the current (directory) node in
`*val`, for descending the tree during iteration. See `obj_readlocal_uint`; uses
`kvs_get_child`.

---

## `obj_lock`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 8caf64c7c26e68d1

Mark the node referenced by `dir` as logically locked (claimed) so that
`obj_removekey` will refuse to delete it.

**Returns:** `CS_OK`; `CS_DNE` on a `kvs` error. This is a per-node *ownership*
flag distinct from the module-global spinlock that serializes calls; it does not
block other readers/writers, it only gates removal. Pair with `obj_unlock`.

---

## `obj_unlock`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 5b70c5ba14757a41

Clear the logical lock set by `obj_lock` on the node referenced by `dir`,
re-allowing its removal.

**Returns:** `CS_OK`; `CS_DNE` on a `kvs` error.

---

## `obj_islocked`

- **kind:** function
- **lang:** c
- **source:** `modules/SysObj/src/main.c`
- **hash:** 306bba1b37be13e2

Report whether the node referenced by `dir` is currently logically locked into
`*status`.

**Returns:** `CS_OK`; `CS_DNE` on a `kvs` error.

---

## `registry_createdirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** cfdd326118b8bbd6

Create a new child directory `dirname` under the existing directory at `path` in
the system registry.

Behaviourally identical to `obj_createdirectory` but operates on the registry
tree. Used at module init to build the standard `HW/...` hierarchy.
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL args; resolver error; `CS_EXISTS`
if present; `CS_FAILURE` on `kvs` failure.

---

## `registry_addkey_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 3b72b74ec80e11c8

Add a new `uint64_t` key to the registry directory at `path`.

The registry `addkey_*`/`readkey_*` families mirror SysObj's exactly (same
locking, same type semantics, same `cs_error` returns); the registry simply has
no `writekey_*` (keys are immutable once added) and no per-node logical
lock/iteration-lock. See `obj_addkey_uint` for the shared semantics. Stored via
`kvs_add_uint`.

---

## `registry_addkey_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 54cdd9c33da09a34

Add a new `ptr`-typed key to the registry. See `registry_addkey_uint`; stored
via `kvs_add_ptr`.

---

## `registry_addkey_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** b2117a8fcbe7ffc9

Add a new signed-`int64_t` key to the registry. See `registry_addkey_uint`;
stored via `kvs_add_sint`.

---

## `registry_addkey_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 97a18ecc9f44ccd1

Add a new string key to the registry, deep-copying `val` into a store-owned,
NUL-terminated buffer (capped at `MAX_REGISTRY_STRLEN-1`).

Identical to `obj_addkey_str`, including the explicit `'\0'` termination needed
because `common`'s `strncpy` neither copies nor pads the terminator and
`registry_readkey_str` later calls `strlen` on the stored pointer. `val` is
dereferenced (`strlen`) before the NULL path/key checks short-circuit, so callers
must pass a non-NULL `val`.

---

## `registry_addkey_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 6658cb0ccdd8f5c0

Add a new boolean key to the registry. See `registry_addkey_uint`; stored via
`kvs_add_bool`.

---

## `registry_readkey_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 7801f865bf89b01d

Read the `uint64_t` value of registry key `keyname` under `path` into `*val`
(NULL `val` probes existence/type only).

See `obj_readkey_uint` for the shared find→type-check→copy semantics under the
global lock. Returns `CS_OK`/`CS_INVALIDARG`/`CS_DNE`/`CS_TYPEMISMATCH`/
`CS_FAILURE`; requires `kvs_val_uint`.

---

## `registry_readkey_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 7334512b4bdfcdd2

Read a `ptr`-typed registry key into `*val`. See `registry_readkey_uint`;
requires `kvs_val_ptr`.

---

## `registry_readkey_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 3dc62e5be83f6c38

Read an `int`-typed (`int64_t`) registry key into `*val`. See
`registry_readkey_uint`; requires `kvs_val_sint`.

---

## `registry_readkey_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 9e83e5951544527c

Read a registry string key into the caller's buffer `val` (capacity `*val_len`),
setting `*val_len` to the full source length.

Identical to `obj_readkey_str`: truncation-safe copy of at most `cap-1` bytes
with an explicit terminator (because `common`'s `strncpy` does not terminate),
and `*val_len` returns the untruncated `strlen` so callers can detect truncation.
Requires `kvs_val_str`.

---

## `registry_readkey_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 3d0f096fa61c4787

Read a `bool`-typed registry key into `*val`. See `registry_readkey_uint`;
requires `kvs_val_bool`.

---

## `registry_removekey`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 3c4f5168d47aebb7

Remove key (or child directory) `keyname` from the registry directory at `path`.

**Parameters:** `path`, `keyname`.
**Returns:** `CS_OK`; `CS_INVALIDARG` for NULL args; `CS_DNE` if absent;
`CS_FAILURE` if `kvs_remove` fails. Unlike `obj_removekey`, the registry has no
per-node logical lock, so removal is unconditional — there is no lock-skip case.

---

## `registry_removedirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 9e70b8576e6a37cb

Remove child directory `dirname` from `path`; a thin alias for
`registry_removekey` (directories and keys share one removal path).

---

## `registry_getdirectory`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 10e3c2420ddffb40

Resolve `path` to its registry node and return an opaque `dir_t` handle in
`*dir` for iteration/local reads. See `obj_getdirectory`.

---

## `registry_next`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 7dd327f8cb487768

Advance the `dir_t` handle in `*dir` to the next sibling entry.

**Returns:** `CS_OK` if advanced; `CS_DNE` at the end. Note: unlike `obj_next`,
the registry `next`/`readlocal_*` family does **not** take the global lock — the
caller is responsible for not mutating the tree concurrently while iterating.

---

## `registry_readlocal_key`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 3b40303752aa97fb

Copy the current handle's entry name into the caller's `keyname` buffer (up to
`MAX_REGISTRY_KEYLEN`). See `obj_readlocal_key`; no global lock taken.

---

## `registry_readlocal_uint`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** bca344c4cbfc25f5

Read the current handle's value as `uint64_t` into `*val`. See
`obj_readlocal_uint`; uses `kvs_get_uint`, no global lock.

---

## `registry_readlocal_ptr`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 9b406c764ca603fa

Read the current handle's value as a pointer into `*val`. See
`registry_readlocal_uint`; uses `kvs_get_ptr`.

---

## `registry_readlocal_int`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** f80c4fc7206634e7

Read the current handle's value as `int64_t` into `*val`. See
`registry_readlocal_uint`; uses `kvs_get_sint`.

---

## `registry_readlocal_str`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** d3ed7df5b854d4fe

Return the current handle's stored string pointer in `*val` (borrowed, no copy).
See `registry_readlocal_uint`; uses `kvs_get_str`.

---

## `registry_readlocal_bool`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** 1b22ac15c75f3b03

Read the current handle's value as `bool` into `*val`. See
`registry_readlocal_uint`; uses `kvs_get_bool`.

---

## `registry_readlocal_dir`

- **kind:** function
- **lang:** c
- **source:** `modules/SysReg/src/main.c`
- **hash:** ed680f3c4b496456

Return a `dir_t` handle to the **first child** of the current (directory) node in
`*val`, for descending the registry tree during iteration. See
`obj_readlocal_dir`; uses `kvs_get_child`.
