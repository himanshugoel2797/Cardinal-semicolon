# Kernel core API

The Cardinal; microkernel core (`kernel/`) does almost nothing on its own: it
loads, verifies, and relocates signed ELF modules and then hands control to
them. The symbols documented here are the *inter-module surface* exported by the
core — the functions other modules resolve against at load time (the ELF
loader, the symbol database, the bootstrap allocator, the initrd reader, the
boot-script interpreter, and the boot-information accessors). Purely internal
static helpers (relocation engine, hash functions, TAR size parsing) are not
documented.

## `elf_installkernelsymbols`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/elf.c`
- **hash:** 243e4929c5379517

Walks the kernel image's own section headers and registers every `STT_FUNC`
symbol into the symbol database so later-loaded modules can resolve against the
core.

Reads the kernel's saved boot information (`GetBootInfo()`) for the ELF section
header table, copies the section headers and the `SHT_SYMTAB`/`SHT_STRTAB`
contents into freshly allocated memory, and rebases their addresses by the
kernel's high-half offset (`0xffffffff80000000`). It then iterates each symbol
table and calls `symboldb_add()` for every function symbol. Must run *before*
any module is loaded — it seeds the symbol DB with the kernel's exports.

**Returns:** `0` on success. Panics (via `symboldb_add`) if the symbol cache
fills.

## `elf_resolvefunction`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/elf.c`
- **hash:** 33eca6d35f7a9d7b

Looks up a symbol by name in the global symbol database and returns its resolved
runtime address, or `NULL` if not found.

**Parameters**
- `name` — NUL-terminated symbol name to resolve.

**Returns:** the symbol's `st_value` (its final relocated address) as a `void *`,
or `NULL` if `name` is `NULL` or no matching symbol is registered. This is the
primary mechanism the core and modules use to call into one another by name
(e.g. the bootstrap allocator wires itself to a real `malloc`/`free`/`realloc`
once `SysMemory` is loaded, and the boot-script `CALL:` directive resolves its
target through this).

## `elf_load`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/elf.c`
- **hash:** 08209bb6a6992178

Validates, relocates, and registers the exported symbols of a relocatable
(`ET_REL`) ELF image in place, returning its `module_init` entry point.

**Parameters**
- `elf` — pointer to the ELF image (the raw relocatable object extracted from a
  `.celf`).
- `elf_len` — length of the image in bytes; must be non-zero.
- `entry_point` — out-param; set to the address of the module's `module_init`
  symbol.

**Returns:** `0` on success, `-1` on a `NULL`/zero-length argument or an invalid
ELF header (the header is checked with `elf_header_valid(hdr, ET_REL)`).

**Behavior / caveats**
- Operates *in place*: section `sh_addr` fields are rewritten to point into the
  loaded image, `SHT_NOBITS`/`SHF_ALLOC` (.bss) sections are `malloc`'d and
  zeroed, and `SHT_REL`/`SHT_RELA` relocations are applied. Unsupported
  relocation types or unresolvable symbols **panic** — there is no graceful
  failure path past header validation.
- Function symbols other than `module_init` are added to the global symbol DB
  via `symboldb_add`, *except* `STV_HIDDEN` or `STB_LOCAL` symbols, which are
  kept private. This is why **load order matters**: a module's imports must
  already be present in the DB.
- Re-loading the same image pointer (tracked in an internal 512-entry table) is
  detected; on a reload it only re-derives the `module_init` entry point and
  skips relocation/registration.

## `symboldb_init`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/symbol_db.c`
- **hash:** 1946107215a541dd

Allocates and zeroes the three parallel hash-table arrays (symbol, section
header, and string-table-header offsets) backing the global symbol database.

Must be called once at bring-up before any `symboldb_add`/`elf_installkernelsymbols`
call. Capacity is fixed at `MAX_SYMBOL_CNT` (65413) entries.

## `symboldb_add`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/symbol_db.c`
- **hash:** 97b8b3e34a9e6bf0

Inserts a symbol into the database keyed by its name, with two-hash open
addressing and weak-symbol override.

**Parameters**
- `strhdr` — the string-table section header for resolving the symbol name.
- `hdr` — the symbol table section header the symbol belongs to.
- `symbol` — the `Elf64_Sym` to register.

**Returns:** `0` on success. Computes an FNV-1a hash of the name and, on
collision with a different name, falls back to a Murmur3 hash for a second slot.
If the existing entry has the *same* name and is `STB_WEAK`, it is overwritten by
the new symbol. **Panics** if the table is full or if both hash slots collide
with distinct names (it cannot store the symbol).

## `symboldb_findmatch`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/symbol_db.c`
- **hash:** 00c9ab06af6cb452

Resolves an undefined symbol from another object against the database by name,
returning the registered definition.

**Parameters**
- `strhdr`, `hdr`, `sym` — the string-table header, symbol-table header, and the
  undefined `Elf64_Sym` to resolve (name is taken from `strhdr` + `sym->st_name`).
- `r_hdr`, `r_sym` — out-params receiving the matching definition's section
  header and symbol.

**Returns:** `0` on a match. Returns `-1` only if the database is empty
(`symbol_cnt == 0`); if the name genuinely is not found in either hash slot it
**panics** ("Could not find symbol"). This is the function the relocation engine
calls to bind `SHN_UNDEF`/`STT_NOTYPE` imports, so an unresolved import is fatal.

## `symboldb_findfunc`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/symbol_db.c`
- **hash:** 8526ce9a0e114916

Looks up a symbol by NUL-terminated name string and returns its database entry,
returning failure (not a panic) when absent.

**Parameters**
- `str` — the symbol name to look up.
- `r_hdr`, `r_sym` — out-params receiving the matching section header and symbol.

**Returns:** `0` on a match, `-1` if any argument is `NULL` or the name is not
found in either the FNV-1a or Murmur3 hash slot. Unlike `symboldb_findmatch`,
this is a soft lookup — it backs `elf_resolvefunction`, which returns `NULL` on
miss.

## `bootstrap_malloc`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/bootstrap_alloc.c`
- **hash:** 48ee05e71682c534

Bump-allocates 16-byte-aligned memory from the kernel's fixed 128 MiB static
bootstrap arena, used before the real heap (`SysMemory`) exists.

**Parameters**
- `s` — requested size in bytes; rounded up to a 16-byte multiple.

**Returns:** a pointer into the static `bootstrap_alloc_area`, or `NULL` if `s`
is `0` or the arena would overflow. Allocation is guarded by a spinlock so it is
SMP-safe. There is no general free list — this is a monotonic bump allocator
(see `bootstrap_free` for the one reclaimable case). The freestanding libc
`malloc`/`free`/`realloc` are weak wrappers that dispatch here until
`kernel_updatememhandlers` rewires them to the loaded heap allocator.

## `bootstrap_free`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/bootstrap_alloc.c`
- **hash:** bf0847680da6c4ff

Releases a bootstrap allocation, but *only* if it is the most recent one (LIFO
bump-pointer rewind).

**Parameters**
- `mem` — pointer previously returned by `bootstrap_malloc`.
- `s` — the original size passed to `bootstrap_malloc` (rounded the same way).

Returns nothing. If `mem` is `NULL`, `s` is `0`, or the block is not exactly the
last allocation (i.e. another allocation happened after it), the call is a no-op
and the memory is permanently leaked within the arena — by design, since the
allocator keeps no per-block metadata.

## `Initrd_GetFile`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/initrd.c`
- **hash:** be77877063abceee

Finds a file in the boot initrd (a USTAR archive) by exact path and returns a
pointer to its contents and length.

**Parameters**
- `file` — exact archive member name to match (e.g. `"./loadscript.txt"`,
  `"./SysMemory.celf"`), compared up to 100 chars (the TAR name field width).
- `loc` — out-param set to a pointer to the file's data (the byte immediately
  after its 512-byte TAR header).
- `size` — out-param set to the file's size in bytes (decoded from the octal TAR
  size field).

**Returns:** `true` if found, `false` if the initrd is absent/empty or the file
is not present. Walks the TAR headers linearly, advancing by 512-byte-aligned
record sizes. The returned pointer is into the in-memory initrd image; do not
free it.

## `module_load`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/load_script.c`
- **hash:** 70b5bad48d8231d1

Loads a signed module (`.celf`) from the initrd by name, relocates it, and runs
its `module_init`.

**Parameters**
- `name` — initrd path of the `.celf` to load.

**Returns:** the value returned by the module's `module_init` (`0` on success).
**Panics** if the file is not found in the initrd or if `elf_load` fails.
Interprets the file as a `ModuleHeader` (`.celf`) and passes `hdr->data` /
`hdr->uncompressed_len` to `elf_load`. (Note: signature verification is the
boundary enforced via `VerifyModule` elsewhere; this function just loads and
invokes.)

## `module_user_load`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/load_script.c`
- **hash:** 6ba0f366511a2e53

Loads a `.celf` from the initrd and starts it as a new userspace task rather than
a kernel-privileged module.

**Parameters**
- `name` — initrd path of the `.celf` to launch in userspace.

Returns nothing. **Panics** if the file is not found. Resolves
`task_startnew_user` through the symbol DB (so the scheduler module must already
be loaded) and hands it the module's ELF payload and uncompressed length. This
backs the boot-script `USER:` directive.

## `loadscript_execute`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/load_script.c`
- **hash:** e584e517c56e15eb

Reads `./loadscript.txt` from the initrd and executes it — the primary boot
sequence that brings up the `Sys*` modules.

**Returns:** `0` on success; **panics** if the script file is missing or any
directive fails. The script is a line-oriented text file of `LOAD:` (load a
`.celf` and run its `module_init`), `CALL:` (resolve and call an exported
function by name), `USER:` (launch a userspace task), and `#` comment lines;
both LF and CRLF line endings are accepted, but an unrecognized command panics
("Unknown Command"). A non-zero return from a `LOAD:` or `CALL:` target panics.

## `apscript_execute`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/load_script.c`
- **hash:** c8b8b9ae4683110e

Reads and executes `./apscript.txt` from the initrd — the per-application-processor
(AP) bring-up sequence used during SMP startup.

**Returns:** `0` on success; same panic-on-failure semantics and directive set as
`loadscript_execute`. Run on each AP as it comes online.

## `servicescript_execute`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/load_script.c`
- **hash:** d065150d9eceb795

Reads and executes `./servicescript.txt` from the initrd using the same boot-script
interpreter.

**Returns:** `0` on success; **panics** if the script is missing. Note: the old C
service/driver binding model that used `servicescript.txt` has been retired in
favor of Lisp (`lisp/init.clp`), so this entry point is effectively legacy — the
function and its `script_execute` interpreter remain, but the default boot no
longer relies on a service script.

## `ParseAndSaveBootInformation`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/platform/x86_64/pc/multiboot2.c`
- **hash:** 22c42e1be636cda4

Parses the bootloader-provided (multiboot2) information blob and populates the
kernel's global `CardinalBootInfo` for later retrieval via `GetBootInfo`.

**Parameters**
- `boot_info` — pointer to the multiboot2 information structure (must be non-NULL).
- `magic` — the bootloader magic value used to validate the protocol.

Returns nothing. Walks the multiboot2 tags to extract memory size and map, the
ELF section header table (for symbol installation), the ACPI RSDP address, the
initrd location/length, the framebuffer description, and the kernel command line.
Called once, very early in `main`, before `GetBootInfo` is used by anything else.

## `GetBootInfo`

- **kind:** function
- **lang:** c
- **source:** `kernel/src/platform/x86_64/pc/multiboot2.c`
- **hash:** d4b943b6ca04233a

Returns a pointer to the kernel's saved `CardinalBootInfo` populated by
`ParseAndSaveBootInformation`.

**Returns:** a pointer to the single static `CardinalBootInfo`. The contents are
only valid after `ParseAndSaveBootInformation` has run. This is the canonical way
core code (ELF symbol install, initrd reader) and modules obtain memory map,
initrd, framebuffer, RSDP, and command-line information.

## `CardinalBootInfo`

- **kind:** struct
- **lang:** c
- **source:** `common/inc/cardinal/boot_info.h`
- **hash:** 528ccd389595125f

The architecture-neutral boot-information record the core fills from the
bootloader and exposes via `GetBootInfo`.

Key fields: `MemorySize` (total RAM); `CardinalMemoryMap` /
`CardinalMemoryMapCount` (the physical memory map); the `elf_shdr_*` fields
describing the kernel's own ELF section header table (used by
`elf_installkernelsymbols`); `RSDPAddress` (ACPI); `InitrdStartAddress` /
`InitrdPhysStartAddress` / `InitrdLength` (the in-memory initrd); the
`Framebuffer*` fields describing the bootloader's linear framebuffer; and
`Cmdline[256]` (the NUL-terminated kernel command line, e.g. `"cardinal.test"`).

## `CardinalMemMap`

- **kind:** struct
- **lang:** c
- **source:** `common/inc/cardinal/boot_info.h`
- **hash:** 778b7684be7597c9

A single physical-memory-map entry referenced by `CardinalBootInfo.CardinalMemoryMap`.

**Fields:** `addr` (base physical address), `len` (length in bytes), and `type`
(a `CardinalMemoryRegionType`). The physical memory manager consumes this array
to seed its free lists.

## `CardinalMemoryRegionType`

- **kind:** enum
- **lang:** c
- **source:** `common/inc/cardinal/boot_info.h`
- **hash:** 8050fe3919efea5a

Classifies a `CardinalMemMap` region.

Currently defines a single value, `CardinalMemoryRegionType_Free` (`1`), marking
a region as usable RAM; anything not so marked is treated as reserved.
