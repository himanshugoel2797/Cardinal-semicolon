# COMMON freestanding mini-libc

Cardinal;'s `common/` tree is a tiny freestanding C support library included as
a SYSTEM header dir everywhere (kernel, modules, servers, drivers). It is **not**
a conforming hosted libc: there is no floating point, no errno, no locale, and
several routines deliberately deviate from ISO C to keep them small and
allocation-free. The most important deviation — `strncpy` does **not** pad or
NUL-terminate the destination — has caused real bugs (`registry` string keys),
so each non-standard routine flags its divergence explicitly below.

Public headers live under `common/inc/`. Most string/number routines are marked
`WEAK` so a platform or module can override them. The allocator declared in
`stdlib.h` (`malloc`/`realloc`/`free`) is only a *prototype* in `common/`; the
live implementation is provided by `SysMemory` (the kernel's bootstrap allocator
backs it before `SysMemory` loads), so those entries point at
`modules/SysMemory/src/allocator.c` — the file whose body is hashed.

---

## `memset`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 851982befb8e2016

Fills the first `n` bytes of `s` with the low 8 bits of `c` and returns `s`.

Writes byte-by-byte until both the pointer and the remaining count are 8-byte
aligned, then bulk-fills 64 bits at a time using a replicated `c` pattern.
Standard behavior; the alignment fast path is an implementation detail.

**Parameters**
- `s` — destination buffer.
- `c` — fill byte (only the low 8 bits are used).
- `n` — number of bytes.

**Returns:** `s`.

**Caveat:** the bulk loop assumes that once the head bytes are consumed the
*remaining* `n` is a multiple of 8; this holds because the head loop only exits
when both pointer and count are aligned. There is no overlap or bounds checking.

---

## `memcpy`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 01eec1457cafa9ef

Copies `n` bytes from `src` to `dest` (which must not overlap) and returns `dest`.

A plain forward byte copy. The arguments are `restrict`-qualified — overlapping
regions are undefined; use `memmove` for those.

**Parameters**
- `dest` — destination (restrict).
- `src` — source (restrict).
- `n` — byte count.

**Returns:** `dest`.

---

## `memcmp`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 3bb306b256b11412

Compares two byte buffers and returns the signed difference of the first
differing bytes, or `0` if the first `n` bytes are equal.

**Non-standard / dangerous:** the loop is `while (*s1 == *s2)` and only stops on
a mismatch or after `--n == 0`. It dereferences `s1`/`s2` **before** checking
whether `n` has reached zero, so calling `memcmp(a, b, 0)` reads one byte past
the intended range. Callers must pass `n >= 1`. The return value uses
`uint8_t` operands, so it is the unsigned-byte difference (matching ISO C's
sign convention), but the zero-length edge case differs from a conforming
`memcmp` (which must return `0` for `n == 0`).

**Parameters**
- `__s1`, `__s2` — buffers to compare.
- `__n` — number of bytes (must be `>= 1`).

**Returns:** `0` if equal over `__n` bytes; otherwise `*s1 - *s2` at the first
mismatch.

---

## `memmove`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 70c3aa73420f64bf

Copies `n` bytes from `src` to `dest`, handling overlap, and returns `dest`.

**Non-standard direction heuristic:** it chooses copy direction by comparing the
raw pointers — if `src < dest` it copies backward (high→low), otherwise forward.
A correct `memmove` decides based on whether the regions actually overlap *and*
in which direction. This implementation is safe for the normal overlap cases
(shifting within one buffer) but the pointer-only test means that for
*non-overlapping* buffers where `src < dest` it still copies backward; that is
harmless for correctness but worth knowing. The parameters are declared
`restrict`, which is misleading for a `memmove` — ignore that qualifier; overlap
is the whole point.

**Parameters**
- `dest` — destination.
- `src` — source.
- `n` — byte count.

**Returns:** `dest`.

---

## `strlen`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** d0ccd6221d0f02b2

Returns the number of bytes before the terminating NUL.

**Non-standard hardening:** returns `0` for a `NULL` argument instead of
crashing (ISO C is undefined here). Otherwise standard.

**Returns:** string length, or `0` if `s == NULL`.

---

## `strnlen`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 99c027c3f1fa71a4

Returns the length of `string` but at most `maxlen`.

Returns `0` for a `NULL` argument (hardened, like `strlen`). Counts up to but
not including the NUL, capped at `maxlen`. Standard POSIX semantics otherwise.

**Parameters**
- `string` — buffer (need not be NUL-terminated within `maxlen`).
- `maxlen` — cap.

**Returns:** `min(strlen(string), maxlen)`, or `0` if `string == NULL`.

---

## `strcmp`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 063d64daf4c1975b

Lexicographically compares two NUL-terminated strings.

**Non-standard sign:** the final difference is computed on `char` operands
(`return *s1 - *s2`), **not** `unsigned char` as ISO C requires. On this target
`char` is signed, so for bytes ≥ 0x80 the sign of the result can differ from a
conforming `strcmp`. Equality (`0`) is always correct; only the ordering of
high-bit bytes is affected. No `NULL` guard — passing `NULL` dereferences it.

**Returns:** `0` if equal; otherwise the (signed-char) difference at the first
mismatch.

---

## `strncmp`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 4a46d2a8254bb110

Compares at most `n` bytes of two strings, stopping at the first NUL.

This one *is* standard (and a prior buggy version that compared all `n` bytes
past a terminator was fixed — see the in-source comment). It stops on mismatch,
on reaching a NUL in `s1`, or after `n` bytes, and returns the `unsigned char`
difference. Use this rather than `strcmp` when a bounded compare is wanted.

**Parameters**
- `s1`, `s2` — strings.
- `n` — max bytes to compare.

**Returns:** `0` if the prefixes match; otherwise the unsigned-byte difference.

---

## `strncpy`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 344cf67b11a22a49

Copies bytes from `src` to `dest`, stopping at the source NUL or after `len`
bytes — **whichever comes first** — and returns `dest`.

**SEVERELY non-standard — read this before using it.** This is **not** ISO C
`strncpy`. It diverges in two load-bearing ways:

1. It does **NOT** NUL-pad the destination when `src` is shorter than `len`
   (ISO C fills the remainder with `\0`).
2. It does **NOT** guarantee the destination is NUL-terminated at all. The copy
   loop is `for (i = 0; i < len && src[i] != 0; i++) dest[i] = src[i];` — it
   simply *stops* at the source NUL and never writes a terminator itself.

Consequences: if `dest` was not pre-zeroed, the copied string runs into whatever
garbage already lived in `dest` past the copied length. This bit the registry
string-key code (`registry_addkey_str` / `readkey_str`), which assumed C
padding. **Always terminate explicitly** after calling: e.g.
`strncpy(d, s, n); d[n-1] = 0;` (or `memset` the buffer first). It behaves more
like a bounded, non-terminating `strlcpy`-without-the-terminator.

**Parameters**
- `dest` — destination (restrict); not zeroed, not terminated by this call.
- `src` — source (restrict).
- `len` — maximum bytes to copy.

**Returns:** `dest`.

---

## `strncat`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 9a339bec250ca58a

Appends at most `count` bytes of `src` to the end of NUL-terminated `dest`,
NUL-terminating the result, and returns `dest`.

Mostly standard `strncat` semantics: it walks to the end of `dest`, copies up to
`count` source bytes (stopping early and returning if it hits the source NUL,
which it copies), and otherwise writes a terminating NUL after `count` bytes. No
bounds checking on the `dest` buffer's capacity — the caller must guarantee room
for the existing contents + `count` + 1.

**Parameters**
- `dest` — NUL-terminated destination to extend (restrict).
- `src` — source to append (restrict).
- `count` — max source bytes to append.

**Returns:** `dest`.

---

## `strchr`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** 67d33b0d6c1aed98

Returns a pointer to the first occurrence of byte `__c` in `__s`, or `NULL`.

**Non-standard in two ways:** (1) it returns `NULL` (hardened) for a `NULL`
input; (2) unlike ISO C, searching for `__c == '\0'` does **not** return a
pointer to the terminator — the loop exits at the NUL and the post-loop check
`*__s == c` happens to match, so a search for `0` *does* in fact return the NUL
position here. Returns `const char *` (ISO C returns non-const `char *`), so the
result cannot be used to mutate the string without a cast.

**Parameters**
- `__s` — string to search.
- `__c` — byte to find (taken as `char`).

**Returns:** pointer to the match (or to the terminator when searching for `0`),
else `NULL`; `NULL` if `__s == NULL`.

---

## `strrchr`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** c77ec165018a2cd5

Returns a pointer to the **last** occurrence of byte `__c` in `__s`, or `NULL`.

**Non-standard / buggy off-by-one:** the loop does `__s++` **before** examining
the character, so it never tests the *first* byte of the string — a match at
index 0 is missed. It also cannot return the terminating NUL position (unlike
ISO C `strrchr(s, '\0')`, which returns a pointer to the NUL). Returns
`const char *`. Hardened against `NULL` (returns `NULL`). Use with care: for
finding e.g. the last `/` in a path it is correct only when the target is not at
position 0.

**Parameters**
- `__s` — string to search.
- `__c` — byte to find.

**Returns:** pointer to the last match (excluding index 0), else `NULL`.

---

## `strstr`

- **kind:** function
- **lang:** c
- **source:** `common/src/memory.c`
- **hash:** a0d48f7c8bcf1ae0

Returns a pointer to the first occurrence of substring `needle` in `haystack`,
or `NULL`.

Hardened: returns `NULL` if either argument is `NULL`, and returns `haystack`
for an empty `needle` (standard). Scans for the first needle character, then
confirms with a bounded `strncmp` of `needle`'s length, bailing early once the
remaining haystack is shorter than the needle. Returns `const char *` (ISO C
returns non-const).

**Parameters**
- `haystack` — string to search in.
- `needle` — substring to find.

**Returns:** pointer to the first match, `haystack` if `needle` is empty, else
`NULL`.

---

## `malloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMemory/src/allocator.c`
- **hash:** af3540732ebdea8d

Allocates `size` bytes from the kernel heap and returns a pointer, or `NULL`.

Declared in `common/inc/stdlib.h`; the live definition is `SysMemory`'s
best-fit free-list allocator (the kernel's bootstrap allocator services it
before `SysMemory` loads, wired up via `elf_resolvefunction`). Sizes are rounded
up to 8 bytes. **Returns `NULL` for a zero-size request** (does not return a
unique pointer). Takes `alloc_lock` under `cli()`, so it is safe to call from
interrupt-disabled contexts. There is no floating point and no alignment
guarantee beyond 8 bytes.

**Parameters**
- `size` — bytes to allocate.

**Returns:** pointer to at least `size` bytes, or `NULL` on failure or `size == 0`.

---

## `realloc`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMemory/src/allocator.c`
- **hash:** 911cdc0d5b77fea3

Resizes a previous allocation, preserving contents, and returns a (possibly
moved) pointer.

Standard C realloc semantics: `realloc(NULL, size)` == `malloc(size)`;
`realloc(ptr, 0)` frees `ptr` and returns `NULL`; if the existing block (after
the same 8-byte rounding `malloc` uses) is already large enough it is returned
in place; otherwise a new block is allocated, the old contents copied, and the
old block freed. **On allocation failure the original block is left intact and
`NULL` is returned** (the caller must not lose its old pointer). Locked under
`cli()` like `malloc`/`free`.

**Parameters**
- `ptr` — existing allocation (or `NULL`).
- `size` — new size (or `0` to free).

**Returns:** pointer to the resized block, or `NULL` on failure / `size == 0`.

---

## `free`

- **kind:** function
- **lang:** c
- **source:** `modules/SysMemory/src/allocator.c`
- **hash:** 24b5f46834b2aa6e

Releases a block previously returned by `malloc`/`realloc`.

`free(NULL)` is a no-op (standard). Marks the block's node free under the
allocator lock. **Double-free is fatal:** freeing an already-free block calls
`PANIC("Double free detected.")` (it records the return address for the
`print_free_addr` debug helper). Memory is not actually returned to the system
yet (compaction/page-return are TODO), only marked reusable.

**Parameters**
- `ptr` — pointer to free, or `NULL`.

---

## `itoa`

- **kind:** function
- **lang:** c
- **source:** `common/src/stdlib.c`
- **hash:** 9cb14bb54a8e19e6

Converts a 32-bit `int` to a NUL-terminated string in the given base, written
into `dst`.

**Non-standard / caveats:** (1) returns `NULL` if `base == 0` or `dst == NULL`;
(2) the `-` sign is only emitted for **base 10** — negative values in other
bases are formatted as their *unsigned* 32-bit bit pattern; (3) digits above 9
use lowercase `a`–`z` (bases up to 36); (4) no buffer-size argument — `dst` must
be large enough (worst case 33 bytes for binary + NUL, 12 for signed decimal).
Builds the string least-significant-digit first then reverses in place.

**Parameters**
- `val` — value to convert.
- `dst` — output buffer (caller-sized).
- `base` — radix (2–36; `0` is rejected).

**Returns:** `dst`, or `NULL` on bad arguments.

---

## `ltoa`

- **kind:** function
- **lang:** c
- **source:** `common/src/stdlib.c`
- **hash:** 1c4bfccfceee27fa

Converts a 64-bit `long long` to a NUL-terminated string in the given base.

Identical algorithm and caveats to `itoa` but for 64-bit values (it casts
through `uint64_t`): `NULL` on `base == 0`/`dst == NULL`, sign only in base 10,
lowercase digits for >9, no size argument. Worst case `dst` size is 65 bytes
(binary + NUL). Widely used for hex pointer dumps in `DEBUG_PRINT`.

**Parameters**
- `val` — value to convert.
- `dst` — output buffer.
- `base` — radix (2–36).

**Returns:** `dst`, or `NULL` on bad arguments.

---

## `atoi`

- **kind:** function
- **lang:** c
- **source:** `common/src/stdlib.c`
- **hash:** 46343a5859290131

Parses an integer from a string — but **only base 16 is implemented**.

**SEVERELY non-standard.** Despite the C-like name, this takes an explicit
`base` argument and **returns `-1` for any base other than 16**. For base 16 it
parses hex digits (`0-9`, `a-f`, `A-F`), stopping at the first non-hex character,
and returns the accumulated `int`. It does **not** skip leading whitespace, does
**not** accept a `0x` prefix or a sign, and has no overflow handling. Do not
expect decimal parsing from this.

**Parameters**
- `ptr` — string to parse (no whitespace/prefix/sign handling).
- `base` — must be `16`; anything else yields `-1`.

**Returns:** the parsed hex value, or `-1` if `base != 16`.

---

## `list_init`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** e155984b4dcb10f3

Zero-initializes a `list_t` doubly-linked list to empty.

Clears the head/tail/cache pointers and the entry count. Must be called before
any other list op. The list stores opaque `void *` values and caches the last
accessed node + index to accelerate sequential `list_at` walks.

**Parameters**
- `list` — list to initialize.

**Returns:** `0`.

---

## `list_append`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** 391593e74d1431d7

Appends a value to the tail of the list, allocating a new node.

`malloc`s a `list_node_t`; on failure returns `list_error_allocation_failed`
without modifying the list. Maintains head/tail links and the access cache.

**Parameters**
- `a` — list.
- `value` — opaque value to store.

**Returns:** `list_error_none` (0) on success, `list_error_allocation_failed`
if the node allocation failed.

---

## `list_len`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** f1100dedb263ea40

Returns the number of entries in the list.

A trivial O(1) read of the cached `entry_count`.

**Parameters**
- `a` — list.

**Returns:** entry count.

---

## `list_at`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** a4b4e2121eb03a90

Returns the value stored at `index`, or `NULL` if out of range.

Walks from the cached last-accessed node toward `index` (forward or backward),
with O(1) fast paths for the first and last elements, then updates the cache.
This makes sequential iteration O(1) per step but random access O(distance).

**Parameters**
- `a` — list.
- `index` — 0-based position.

**Returns:** the stored `void *`, or `NULL` if `index >= len`.

---

## `list_remove`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** f324d0ca21b57eb4

Removes the node at `index`, freeing it.

No-op if the list is empty or `index` is out of range. Re-uses the access cache
to navigate to the node, unlinks it, `free`s the node (not the stored value —
the caller owns the value's lifetime), resets the cache to the head, and
decrements the count.

**Parameters**
- `a` — list.
- `index` — position to remove.

---

## `list_remove_value`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** 079fb67d1c17a10e

Removes the first node whose stored value pointer equals `val`.

Linear scan via `list_at`; on the first pointer-equal match it calls
`list_remove` and returns. **Acquires no lock** — the caller must already hold
whatever lock protects the list (see the header comment).

**Parameters**
- `l` — list.
- `val` — value pointer to match by identity.

**Returns:** `1` if a match was found and removed, `0` if not found.

---

## `list_fini`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** de79095c21ec7464

Empties the list, freeing every node.

Repeatedly `list_remove(a, 0)` until empty. Frees the nodes only; stored values
are not freed (caller-owned).

**Parameters**
- `a` — list.

---

## `list_rot_next`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** 24bd4312d4f631b8

Advances the access cache one step forward (wrapping) and returns the value
there, giving a cheap circular iterator.

Computes `(last_accessed_index + 1) % entry_count` and returns `list_at` of it.
Returns `NULL` if the list is empty.

**Parameters**
- `a` — list.

**Returns:** the next value in rotation, or `NULL` if empty.

---

## `list_rot_prev`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** 7e661ecd17667e01

Retreats the access cache one step backward (wrapping) and returns the value
there.

Computes `(last_accessed_index - 1) % entry_count` and returns `list_at` of it.

**Caveat:** the index is *unsigned*, so when the cache is at index 0 the `- 1`
wraps to a huge value before the modulo; the intent (step to the last element)
relies on that unsigned wraparound landing back in range. Returns `NULL` if the
list is empty.

**Parameters**
- `a` — list.

**Returns:** the previous value in rotation, or `NULL` if empty.

---

## `list_history`

- **kind:** function
- **lang:** c
- **source:** `common/src/list.c`
- **hash:** c7cfa6708df7bf5f

Returns the index the access cache currently points at.

Exposes `last_accessed_index` — useful with `list_rot_next`/`list_rot_prev` to
know where the circular iterator sits.

**Parameters**
- `a` — list.

**Returns:** the cached last-accessed index.

---

## `queue_init`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** e1786a3c39ea9487

Allocates the backing storage for a fixed-capacity ring queue of `uint64_t`s.

`malloc`s `sz` slots. Returns `-1` if `q` is `NULL` or the allocation fails,
else `0`. The queue is a single-producer/single-consumer ring using atomic
head/tail indices. **Usable capacity is `sz - 1`** (one slot is kept empty to
distinguish full from empty).

**Parameters**
- `q` — queue struct to initialize.
- `sz` — number of slots to allocate.

**Returns:** `0` on success, `-1` on `NULL` queue or allocation failure.

---

## `queue_fini`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 95a1c8371eaed72d

Frees the queue's backing storage.

No-op if `q` is `NULL`. Frees the slot array and zeroes the size. Does not free
the `queue_t` struct itself.

**Parameters**
- `q` — queue.

---

## `queue_size`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** b4334a713e3bc991

Returns the allocated slot capacity of the queue.

Returns `0` for a `NULL` queue. Note this is the raw `sz` passed to
`queue_init`; the number of items that can be held is `size - 1`.

**Parameters**
- `q` — queue.

**Returns:** allocated capacity, or `0` if `q == NULL`.

---

## `queue_entcnt`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 195563488cb55b19

Returns the current number of queued items.

Reads the atomic `ent_cnt`. Returns `0` for a `NULL` queue.

**Parameters**
- `q` — queue.

**Returns:** number of items currently enqueued.

---

## `queue_full`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 6610b638687e6f0c

Reports whether the queue is at capacity.

Returns `true` when `queue_entcnt(q) == queue_size(q)`. **Caveat:** because the
ring keeps one slot empty, `tryenqueue` actually fails one item *before*
`ent_cnt` reaches `size`; this predicate compares against `size`, not `size - 1`,
so it can report not-full when the next enqueue will in fact fail. Prefer
checking the `queue_tryenqueue` return value.

**Parameters**
- `q` — queue.

**Returns:** `true` if `ent_cnt == size`.

---

## `queue_tryenqueue`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** bd2ece4f67a242f2

Appends a value to the tail of the ring, returning `false` if full.

Single-producer enqueue: fails (returns `false`) on `NULL` queue or when the
ring is full (head one ahead of tail+1, i.e. only `size - 1` usable slots).
Writes the slot, advances the atomic tail, increments the count.

**Parameters**
- `q` — queue.
- `val` — 64-bit value to enqueue.

**Returns:** `true` if enqueued, `false` if full or `q == NULL`.

---

## `queue_tryenqueue_front`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 98c37e2a2c28e172

Pushes a value onto the **head** of the ring (LIFO-style), returning `false` if
full.

Same fullness check as `queue_tryenqueue`, but it decrements the head (wrapping
to `size - 1` from 0) and stores at the new head, so the value will be the next
one dequeued. Lets a consumer "un-read" or prioritize an item.

**Parameters**
- `q` — queue.
- `val` — value to push to the front.

**Returns:** `true` if pushed, `false` if full or `q == NULL`.

---

## `queue_trydequeue`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 8cd1bcf2999f7dfe

Removes and returns the head value, returning `false` if empty.

Single-consumer dequeue: returns `false` on `NULL` queue or when empty
(head == tail). On success writes the value through `val`, zeroes the slot,
advances the atomic head, decrements the count.

**Parameters**
- `q` — queue.
- `val` — out-pointer for the dequeued value.

**Returns:** `true` if an item was dequeued, `false` if empty or `q == NULL`.

---

## `queue_peek`

- **kind:** function
- **lang:** c
- **source:** `common/src/queue.c`
- **hash:** 9a2f8e1019b14bf5

Reads the head value without removing it, returning `false` if empty.

Returns `false` on `NULL`/empty queue; otherwise copies the head slot through
`val` and leaves the queue unchanged.

**Parameters**
- `q` — queue.
- `val` — out-pointer for the peeked value.

**Returns:** `true` if a value was read, `false` if empty or `q == NULL`.

---

## `gmtime`

- **kind:** function
- **lang:** c
- **source:** `common/src/time.c`
- **hash:** 2618431e16f7f5af

Converts a UTC `time_t` (seconds since the 1970 epoch) into a broken-down
`struct tm`.

**Non-reentrant:** returns a pointer to a single `static struct tm`, so each
call overwrites the previous result (standard `gmtime` behavior, but note it in
multi-core code). Computes weekday, year (walking from 1970), day-of-year,
month, and day with leap-year handling. Designed for post-epoch times — the only
case the kernel produces; negative inputs are normalized so `0 <= rem < 86400`
but the pre-1970 calendar walk is best-effort. `tm_isdst` is always `0`.

**Parameters**
- `a` — pointer to the `time_t` to convert.

**Returns:** pointer to a static `struct tm` (overwritten on each call).

---

## `strftime`

- **kind:** function
- **lang:** c
- **source:** `common/src/time.c`
- **hash:** 8e4e895a8c3f1bea

Formats a `struct tm` into `__s` per a `strftime`-style `__format`, returning the
character count (excluding NUL) or `0` if it did not fit.

**A practical subset of ISO C `strftime`.** Supported specifiers:
`%Y %y %m %d %e %H %M %S %j %p %a %b %%`. An **unknown** `%x` is emitted
verbatim as the two characters `%x` (not undefined). A trailing lone `%`
produces a literal `%`. Always NUL-terminates on success. Returns `0` (and
leaves the buffer contents unspecified) if the output would not fit in
`__maxsize`, or if `__maxsize == 0`. Weekday/month names are 3-letter English
abbreviations; locale is not honoured.

**Parameters**
- `__s` — output buffer (restrict).
- `__maxsize` — buffer size including room for the NUL.
- `__format` — format string (restrict).
- `__tp` — broken-down time (restrict).

**Returns:** number of characters written excluding the terminating NUL, or `0`
on overflow / `__maxsize == 0`.

---

## `fnv1a_hash`

- **kind:** function
- **lang:** c
- **source:** `common/inc/hash.h`
- **hash:** b5c18b9f44f37b3c

Computes the 32-bit FNV-1a hash of a byte buffer.

A `static inline` header-only function (so it is inlined into each user; the
hashed body lives in the header). Standard FNV-1a: start at `FNV1A_BASIS`
(`2166136261`), and for each byte XOR it in then multiply by `FNV1A_PRIME`
(`16777619`). Shared so the kernel symbol DB and `libs/kvs` need not each carry
a copy (the kernel cannot link against `libs/`).

**Parameters**
- `src` — input bytes (not required to be NUL-terminated).
- `src_len` — number of bytes to hash.

**Returns:** the 32-bit FNV-1a hash.

---

## `local_spinlock_lock`

- **kind:** function
- **lang:** c
- **source:** `common/inc/cardinal/local_spinlock.h`
- **hash:** 2bc01042e72e3abc

Acquires a simple test-and-set spinlock, busy-waiting until it is free.

A `static inline` header-only primitive. Spins on `__sync_lock_test_and_set`
(acquire barrier); while the lock is held it inner-spins reading the value and
issuing `pause` to relax the CPU. **Not** recursive and **not** interrupt-safe
by itself — callers that need to block preemption pair this with `cli()`/`sti()`
(see `SysMemory`'s allocator). The lock word is an `int` owned by the caller.

**Parameters**
- `x` — pointer to the caller-owned lock word.

---

## `local_spinlock_trylock`

- **kind:** function
- **lang:** c
- **source:** `common/inc/cardinal/local_spinlock.h`
- **hash:** c2c1038a13333190

Attempts to acquire the spinlock once without blocking.

`static inline`. Returns `true` if it took the lock (the prior value was 0),
`false` if it was already held. No busy-wait.

**Parameters**
- `x` — pointer to the lock word.

**Returns:** `true` on acquisition, `false` if already locked.

---

## `local_spinlock_unlock`

- **kind:** function
- **lang:** c
- **source:** `common/inc/cardinal/local_spinlock.h`
- **hash:** f2bc037fd5c9505b

Releases the spinlock.

`static inline`; calls `__sync_lock_release` (release barrier) to store 0 into
the lock word. Must be called by the holder; there is no ownership check.

**Parameters**
- `x` — pointer to the lock word.

---
