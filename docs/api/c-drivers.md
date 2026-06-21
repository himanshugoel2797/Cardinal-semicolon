# C device drivers

API documentation for the remaining **C** device drivers in Cardinal;. The USB
stack (UHCI/xHCI + HID/hub/storage class drivers) and the `ps2` keyboard driver
are now Lisp and are documented elsewhere; this file covers only the drivers that
are still native C: the shared **virtio** infrastructure and the **virtio GPU**
class driver, the **AHCI** SATA controller, the **rtl8139** NIC, the **lfb**
linear-framebuffer display, and the **tarfs** / **cardfs** filesystem providers.

Most cross-module symbols here are declared `PRIVATE` (module-local visibility in
the relocatable model) — they form the driver's *internal* surface shared across
its own translation units, not a kernel-wide export. The one true public contract
of every PCI driver is its `module_init` entry point, which the boot policy in
`lisp/init.clp` invokes with the device's ECAM config space.

---

## `virtio_initialize`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 3bd6a18c685914b6

Maps a virtio-pci device's ECAM, walks its capability list to locate the common /
notify / ISR / device config structures, performs the device reset + ACKNOWLEDGE/
DRIVER status handshake, sets up MSI, and returns an allocated `virtio_state_t`.

**Parameters**
- `ecam_addr` — physical address of the device's PCI ECAM config space (as passed
  to the driver's `module_init`).
- `int_handler` — the driver's interrupt handler, signature `void(*)(int)`,
  registered for the device's MSI vector.
- `cmds` — per-queue array of `virtio_virtq_cmd_state_t *` command-state tables the
  driver owns (one entry per virtqueue); used to track in-flight requests.
- `avail_idx`, `used_idx` — per-queue driver-side ring index counters owned by the
  driver; `virtio_state_t` keeps pointers to them.

**Returns** a heap-allocated `virtio_state_t *` wired to the device's config
regions, or aborts on a fatal mapping/capability error. The caller then negotiates
features (`virtio_getfeatures`/`virtio_setfeatures`/`virtio_features_ok`), sets up
queues (`virtio_setupqueue`), and finishes with `virtio_driver_ok`.

---

## `virtio_getfeatures`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 8612d66d4291bf3b

Reads one 32-bit word of the device's offered feature bits.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — feature-word select (0 for the low 32 features, 1 for the next word).

**Returns** the `device_feature` register for the selected word. Writes
`device_feature_select = idx` first, then reads.

---

## `virtio_setfeatures`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 1a8663e2dd5aa733

Writes the driver-accepted feature bits for one 32-bit feature word.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — feature-word select (matches the `virtio_getfeatures` index).
- `val` — the subset of offered features the driver accepts.

Sets `driver_feature_select = idx` then `driver_feature = val`. Call once per
feature word before `virtio_features_ok`.

---

## `virtio_features_ok`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** bb03a0e9989e05cf

Sets the `FEATURES_OK` status bit and re-reads it to confirm the device accepted
the negotiated feature set.

**Parameters**
- `state` — the device's `virtio_state_t`.

**Returns** `true` if the device still has `FEATURES_OK` set after the write (the
negotiated feature set is acceptable), `false` if the device cleared it (the driver
must not proceed). Call after all `virtio_setfeatures` calls and before
`virtio_setupqueue`.

---

## `virtio_driver_ok`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 6bd6206a929e7d68

Sets the `DRIVER_OK` status bit, signalling the device that the driver is fully
initialized and queues are live.

**Parameters**
- `state` — the device's `virtio_state_t`.

Call last, after queues are set up; the device may begin using the virtqueues once
this is set.

---

## `virtio_setupqueue`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** be57a4d2ef887498

Selects a virtqueue, sizes it, allocates the contiguous descriptor/avail/used ring
DMA region, programs the device's queue address registers, and enables the queue.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index to configure (`queue_select`).
- `entcnt` — number of descriptor entries (queue size; power of two).

**Returns** the virtual address of the allocated virtqueue region (descriptors at
offset 0, avail ring after `16*entcnt`, used ring after that), or `NULL` if the
physical allocation failed. Programs `queue_desc`/`queue_avail`/`queue_used` with
the physical addresses, sets `queue_msix_vector = 0`, and `queue_enable = 1`.

---

## `virtio_notify`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** facbc2ade1041ef9

Kicks the device by writing the queue index to its per-queue notify register,
telling it new descriptors are available.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index to notify.

Computes the notify address from the notify-capability BAR, the cap offset, the
queue's `queue_notify_off`, and the `notify_multiplier`.

---

## `virtio_addresponse`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 94e464ecc6ddbf69

Posts a single device-writable (response-only) descriptor into a queue's available
ring, recording its command-state for later completion.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index.
- `buf` — virtual address of the response buffer the device will write into.
- `len` — buffer length in bytes.
- `resp_handler` — completion callback invoked when the device returns the
  descriptor via the used ring.

Allocates the next descriptor slot, translates `buf` to a physical address, marks
the descriptor `VIRTQ_DESC_F_WRITE`, and bumps the avail-ring index. Used for
queues that only receive (e.g. an RX or event queue).

---

## `virtio_postcmd_noresp`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 42aeceae9af3c3a7

Posts a single device-readable command descriptor (no response buffer) into a
queue's available ring.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index.
- `cmd` — virtual address of the command buffer the device will read.
- `len` — command length in bytes.
- `resp_handler` — completion callback for when the descriptor is returned via the
  used ring.

Allocates the next descriptor, translates `cmd` to physical, marks it read-only
(flags 0), and bumps the avail index. Use `virtio_notify` afterward to kick the
device.

---

## `virtio_postcmd`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** 5406417cfddad32c

Posts a request/response pair — a device-readable command descriptor chained to a
device-writable response descriptor — into a queue's available ring.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index.
- `cmd` — virtual address of the command buffer (read by the device).
- `len` — command length in bytes (must be non-zero; the code asserts on zero).
- `resp` — virtual address of the response buffer (written by the device); may be
  unused when `response_len` is 0.
- `response_len` — response buffer length; when `> 0` a second `VIRTQ_DESC_F_WRITE`
  descriptor is chained via `VIRTQ_DESC_F_NEXT`.
- `resp_handler` — completion callback invoked when the device finishes the request.

Consumes two descriptor slots (advances the avail index by 2). This is the normal
request/response path used by the GPU control queue.

---

## `virtio_accept_used`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/src/virtio_main.c`
- **hash:** c3501e1459ce1b04

Drains a queue's used ring, marking each returned command's state `finished`,
recording the device-written byte count, and invoking its completion handler.

**Parameters**
- `state` — the device's `virtio_state_t`.
- `idx` — queue index to drain.

Called from the driver's poll loop / ISR after `virtio_poll_signal`. Walks
`used_idx[idx]` up to the device's current used index, copying `used_len` into each
`virtio_virtq_cmd_state_t` and dispatching its `handler`.

---

## `virtio_poll_signal`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/inc/virtio.h`
- **hash:** 87b5b2e5791e851b

Inline ISR helper that sets the pending-event flag on a `virtio_poll_state_t` so
the driver's poll loop wakes up.

**Parameters**
- `ps` — the driver instance's `virtio_poll_state_t`.

Call from the driver's MSI handler (`void(*)(int)`). It only sets `signalled`; the
actual work runs in `virtio_run_poll_loop` under the queue lock.

---

## `virtio_run_poll_loop`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/common/inc/virtio.h`
- **hash:** d41767d0b14e6db1

Inline never-returning task body that waits for the instance to be initialized,
then repeatedly drains pending events under the queue spinlock by calling the
driver's poll callback.

**Parameters**
- `ps` — the driver instance's `virtio_poll_state_t`.
- `poll_cb` — device-specific work callback (typically calls `virtio_accept_used`);
  must not touch `ps->signalled` or `ps->queue_avl` itself.
- `ctx` — opaque context passed to `poll_cb`.

Blocks until `ps->inited`, then loops: take `ps->queue_avl`, clear `ps->signalled`,
run `poll_cb(ctx)`, release the lock. Each virtio driver runs this in its own
kernel task because the GPU and net drivers are independently-loaded `.celf`
modules that each own their own `virtio_poll_state_t`.

---

## `virtio_gpu_submitcmd`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 5bc5fd31b9b715d3

Generic helper that submits a GPU command on a chosen virtqueue, optionally with a
response buffer, and kicks the device.

**Parameters**
- `q` — virtqueue index (0 = control queue, 1 = cursor queue).
- `cmd` — command structure (a `virtio_gpu_*` request).
- `cmd_len` — command length in bytes.
- `resp` — response buffer, or `NULL`.
- `resp_len` — response length (0 for no response).
- `resp_handler` — completion callback; the GPU passes `virtio_gpu_default_handler`
  for fire-and-forget commands.

Wraps `virtio_postcmd` / `virtio_postcmd_noresp` plus `virtio_notify` against the
GPU's shared `virtio_state_t`. The other `virtio_gpu_*` builders below all funnel
through this.

---

## `virtio_gpu_default_handler`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** e068356f4b2c81c5

Default GPU command-completion callback that marks the command slot as the latest
ready command.

**Parameters**
- `cmd` — the completed `virtio_virtq_cmd_state_t`.

Used as the `resp_handler` for commands whose result the caller does not need to
parse (create/unref/flush/transfer); records completion so the submit path can
recycle the descriptor.

---

## `virtio_gpu_getdisplayinfo`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 77811bee133758ce

Issues `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` to query the attached scanouts'
resolutions and enabled state.

**Parameters**
- `handler` — completion callback receiving a `virtio_gpu_resp_display_info_t`
  response; the driver passes `virtio_gpu_displayinit_handler` during bring-up.

The response's `pmodes[]` array carries up to `VIRTIO_GPU_MAX_SCANOUTS` display
rectangles. `module_init` blocks on this during init before registering with
CoreDisplay.

---

## `virtio_gpu_create2d`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** ffcba775c5ca0e4a

Issues `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` to create a host-side 2D resource of the
given format and size.

**Parameters**
- `id` — resource id to assign.
- `fmt` — pixel format (`virtio_gpu_formats_t`, e.g. `VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM`).
- `width`, `height` — resource dimensions in pixels.

Pair with `virtio_gpu_attachbacking` to give the resource guest memory, then
`virtio_gpu_setscanout` to bind it to a display.

---

## `virtio_gpu_unref`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 531ebc339f53eff6

Issues `VIRTIO_GPU_CMD_RESOURCE_UNREF` to destroy a host-side 2D/3D resource.

**Parameters**
- `rsc_id` — resource id to release.

Frees the host resource; detach any backing first if needed.

---

## `virtio_gpu_setscanout`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 1e7141df0a9f56ff

Issues `VIRTIO_GPU_CMD_SET_SCANOUT` to bind a resource (and a sub-rectangle of it)
to a physical display output.

**Parameters**
- `scanout_id` — target scanout (display) index.
- `resource_id` — resource to scan out (0 disables the scanout).
- `x`, `y`, `w`, `h` — the rectangle of the resource shown on that scanout.

After binding, use `virtio_gpu_transfertohost2d` + `virtio_gpu_flush` to present
guest framebuffer contents.

---

## `virtio_gpu_flush`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 46dfa76932ceb018

Issues `VIRTIO_GPU_CMD_RESOURCE_FLUSH` to push a resource rectangle to the screen.

**Parameters**
- `resource_id` — the resource to flush.
- `x`, `y`, `w`, `h` — the dirty rectangle to present.

Call after `virtio_gpu_transfertohost2d` has copied guest memory into the host
resource; flush is what makes the update visible.

---

## `virtio_gpu_transfertohost2d`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** a1d340ac58f587ed

Issues `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` to copy a rectangle from the resource's
guest backing memory into the host-side resource.

**Parameters**
- `resource_id` — destination resource.
- `offset` — byte offset into the backing memory where the rectangle's data starts.
- `x`, `y`, `w`, `h` — the rectangle to transfer.

Step 1 of presenting a frame; `virtio_gpu_flush` is step 2.

---

## `virtio_gpu_attachbacking`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 80cc2163b9b50b54

Issues `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING` to give a resource a single
contiguous guest-physical memory region as its backing store.

**Parameters**
- `rsc_id` — the resource to attach backing to.
- `addr` — guest-physical base address of the backing memory.
- `len` — backing length in bytes.

Sends one `virtio_gpu_mem_entry_t`. Required before transfers can copy guest data
into the resource.

---

## `virtio_gpu_detachbacking`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** dcb5cccaa2bb928d

Issues `VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING` to remove a resource's backing
memory.

**Parameters**
- `rsc_id` — the resource whose backing is detached.

---

## `virtio_gpu_ctx_create`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 033cb63382571dd3

Issues `VIRTIO_GPU_CMD_CTX_CREATE` to create a 3D (virgl) rendering context.

**Parameters**
- `ctx_id` — context id to create.
- `name` — 64-byte debug name for the context.

Only meaningful when the device advertised `VIRTIO_GPU_F_VIRGL` and the driver is
in `virgl_mode`.

---

## `virtio_gpu_ctx_destroy`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** a0b53f7d8ea53d1c

Issues `VIRTIO_GPU_CMD_CTX_DESTROY` to destroy a 3D context.

**Parameters**
- `ctx_id` — context id to destroy.

---

## `virtio_gpu_ctx_attachresource`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** cb51b7cd600f560f

Issues `VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE` to make a resource usable from a 3D
context.

**Parameters**
- `ctx_id` — the 3D context.
- `handle` — resource handle to attach.

---

## `virtio_gpu_ctx_submit`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** fb0074d4c4c94259

Issues `VIRTIO_GPU_CMD_SUBMIT_3D` to submit a virgl 3D command buffer to a context.

**Parameters**
- `ctx_id` — the 3D context the commands run in.
- `cmds` — pointer to the virgl command-buffer words.
- `sz` — command-buffer size in bytes.

The 3D rendering fast path; only valid in `virgl_mode`.

---

## `virtio_gpu_displayinit_handler`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** a77215f1aa76b30c

Completion handler for the initial `virtio_gpu_getdisplayinfo`; reads the reported
modes, allocates a backing framebuffer, creates the scanout-0 resource, and binds
it so the GPU can be registered with CoreDisplay.

**Parameters**
- `cmd` — the completed display-info command carrying the response buffer.

`module_init` spins until scanout 0 has a resource id (set by this handler) before
calling `display_register`.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/virtio/gpu/src/main.c`
- **hash:** 5c35b07d3ef66b1a

Entry point for the **virtio-gpu** PCI driver: brings up the device, negotiates VirGL,
sets up the control and cursor queues, queries display info, and registers the GPU
as a CoreDisplay display.

**Parameters**
- `ecam` — the device's PCI ECAM config space (passed by the boot device binder).

**Returns** 0 on success. Spawns the `virtio_gpu_0` kernel poll task, calls
`virtio_initialize`, negotiates `VIRTIO_GPU_F_VIRGL` into `virgl_mode`, sets up two
virtqueues (`VIRTIO_GPU_VIRTQ_LEN` each), then blocks on `virtio_gpu_getdisplayinfo`
before `display_register`. Returns early (also 0) if feature negotiation fails.
The symbol's actual C name is `module_init`.

---

## `ahci_read8`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 9ead9073c05f21db

8-bit MMIO register read accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is read.
- `off` — register byte offset (e.g. `HBA_GHC`, `HBA_PxIS(port)`).

**Returns** the byte at `inst->cfg + off`. `ahci_read16`/`ahci_read32` are the
16/32-bit counterparts (same signature, wider return). Use the `HBA_*` offset
macros from `ahci.h` to address the global HBA and per-port register blocks.

---

## `ahci_read16`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 5057ea6187e86afe

16-bit MMIO register read accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is read.
- `off` — register byte offset.

**Returns** the 16-bit value at `inst->cfg + off`.

---

## `ahci_read32`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** c49a1f783a3e6fe5

32-bit MMIO register read accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is read.
- `off` — register byte offset.

**Returns** the 32-bit value at `inst->cfg + off`; the workhorse accessor for the
GHC, interrupt-status, and command-issue registers.

---

## `ahci_write8`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 16849a535cc80736

8-bit MMIO register write accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is written.
- `off` — register byte offset.
- `val` — value to store.

`ahci_write16`/`ahci_write32` are the 16/32-bit counterparts.

---

## `ahci_write16`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** dab02708316266cb

16-bit MMIO register write accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is written.
- `off` — register byte offset.
- `val` — value to store.

---

## `ahci_write32`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** a1b7ed08934aaa38

32-bit MMIO register write accessor for an AHCI HBA instance.

**Parameters**
- `inst` — the `ahci_instance_t` whose mapped config base is written.
- `off` — register byte offset.
- `val` — value to store.

Counterpart to the `ahci_read*` accessors; used to program GHC, port command,
interrupt-enable, and command-issue registers.

---

## `ahci_resethba`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 5e19961e8c51bc52

Performs an HBA reset via the GHC `HBA_RESET` bit and waits for it to clear.

**Parameters**
- `inst` — the HBA instance to reset.

Called during `module_init` after BIOS/OS ownership handoff to put the controller
into a known state before port init.

---

## `ahci_readports`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 92fdef3c4942d14a

Reads the HBA's Ports-Implemented (`HBA_PI`) bitmap.

**Parameters**
- `inst` — the HBA instance.

**Returns** the 32-bit mask of implemented ports; `module_init` iterates set bits
to call `ahci_initializeport` per port.

---

## `ahci_obtainownership`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** cbd516694bc328c1

Performs the BIOS/OS handoff (BOHC) to take controller ownership from firmware.

**Parameters**
- `inst` — the HBA instance.

Part of the bring-up sequence in `module_init`; runs before `ahci_resethba`.

---

## `ahci_reportawareness`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 15602752e3890fc4

Sets the GHC AHCI-Enable bit so the controller interprets registers in AHCI
(non-legacy-IDE) mode.

**Parameters**
- `inst` — the HBA instance.

Called both before and after the HBA reset in `module_init`.

---

## `ahci_initializeport`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 8427f7efc51a550a

Initializes one AHCI port: stops the command engine, assigns its slice of the DMA
region as the command-list and received-FIS areas, and (if a device is present)
restarts the FIS-receive and command engines.

**Parameters**
- `inst` — the HBA instance.
- `index` — port number.

**Returns** 0 if a device is present and the port was brought up, non-zero
otherwise. The command-list / FIS / command-table DMA all come from the per-HBA
contiguous region in `inst->port_dma`.

---

## `ahci_getcmdslot`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** b1a8dd10d99ebc80

Finds a free command slot on a port by scanning the SACT/CI registers against the
driver's active-command bitmap.

**Parameters**
- `inst` — the HBA instance.
- `index` — port number.

**Returns** a free slot index, or a negative value if all 32 slots are busy.

---

## `ahci_readdev`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/ahci.c`
- **hash:** 93387d1e228a4afc

Reads `len` bytes from a SATA device starting at LBA `loc` into `addr` via a
DMA `READ DMA EXT` command on a free command slot.

**Parameters**
- `inst` — the HBA instance.
- `index` — port number to read from.
- `loc` — starting LBA.
- `addr` — destination buffer (its physical pages become the command's PRDT
  entries).
- `len` — number of bytes to read.

**Returns** 0 on success (command issued/completed), negative on failure. Builds a
host-to-device register FIS and PRDT in the port's command table, issues it via
`HBA_PxCI`, and waits for the completion bit the ISR records.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/ahci/src/main.c`
- **hash:** 75a7ff0c73f51d25

Entry point for the **AHCI** PCI driver: maps the device, enables bus mastering and
MSI, resets and takes ownership of the HBA, allocates the port DMA region, and
initializes every implemented port.

**Parameters**
- `ecam_addr` — the device's PCI ECAM config space (passed by the boot device
  binder).

**Returns** 0 on success, -1 if the port DMA allocation fails. Sets up the MSI
handler (`tmp_handler`, which scans `HBA_IS`/`HBA_PxIS` and tracks finished
commands), enables HBA interrupts last, and links the new `ahci_instance_t` into
the global instance list (supporting multiple HBAs). The symbol's actual C name is
`module_init`.

---

## `rtl8139_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/rtl8139/src/driver.c`
- **hash:** 3db30dc10814869e

Resets and configures an RTL8139 NIC: powers it on, performs a soft reset,
allocates the RX/TX DMA buffers, programs the receive-config and interrupt-mask
registers, reads the MAC, enables RX/TX, and registers with CoreNetwork.

**Parameters**
- `state` — a zeroed `rtl8139_state_t` with `memar` already pointing at the mapped
  register BAR.

**Returns** 0 on success. After this returns the caller spawns a kernel task
running `rtl8139_intr_handler`. Note (per the repo memory): rtl8139's RX path is
incomplete — use virtio-net for live networking tests.

---

## `rtl8139_intr_handler`

- **kind:** function
- **lang:** c
- **source:** `drivers/rtl8139/src/driver.c`
- **hash:** a4ddf5f2b9e4187e

The RTL8139 interrupt/poll task body: services the ISR register, draining received
frames into CoreNetwork and acknowledging TX completion.

**Parameters**
- `state` — the NIC's `rtl8139_state_t`.

Run as a dedicated kernel task (`rtl8139_int_poll`) started by `module_init`.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/rtl8139/src/main.c`
- **hash:** 1666c55519b7b23b

Entry point for the **RTL8139** PCI driver: maps the register BAR, enables bus
mastering, allocates and initializes the NIC, and spawns its interrupt-poll task.

**Parameters**
- `ecam_addr` — the device's PCI ECAM config space (passed by the boot device
  binder).

**Returns** 0 on success. Maps BAR1 (the memory-mapped register window), calls
`rtl8139_init`, then `task_create_kernel`/`task_start_kernel` to run
`rtl8139_intr_handler`. The symbol's actual C name is `module_init`.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/lfb/src/main.c`
- **hash:** 2792ebc165db50cf

Entry point for the **lfb** linear-framebuffer display driver: reads the boot-provided
framebuffer geometry from the registry and registers a single fixed-mode display
with CoreDisplay.

**Parameters** none (this is a plain module, not a PCI driver — it has no `ecam`).

**Returns** 0 on success. Reads `HW/BOOTINFO/FRAMEBUFFER` registry keys
(`PHYS_ADDR`, `PITCH`, `WIDTH`, `HEIGHT`), then `display_register`s a
`display_desc_t` ("Linear Framebuffer") whose `get_framebuffer` handler maps the
framebuffer physical address uncached and whose `get_displayinfo` reports the fixed
resolution at 60 Hz. `set_resolution`/`set_brightness`/`flush` are unsupported
(NULL). The symbol's actual C name is `module_init`.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/tarfs/src/main.c`
- **hash:** 9a7f0046d1b21929

Entry point for the **tarfs** module — currently a stub that does nothing.

**Parameters** none.

**Returns** 0 unconditionally. The body is empty: tarfs registers no filesystem
provider yet (the initrd tar is parsed by the kernel itself). Listed in
`notes/AUDIT.md` as an unfinished `module_init`. The symbol's actual C name is
`module_init`.

---

## `module_init`

- **kind:** function
- **lang:** c
- **source:** `drivers/cardfs/src/main.c`
- **hash:** 419072374ff3f74c

Entry point for the **cardfs** object-store driver: registers a CoreStorage filesystem
provider whose probe mounts an existing cardfs volume (read-only by default).

**Parameters** none.

**Returns** 0. Fills a `storage_fsprovider_t` named "cardfs" with the `cardfs_probe`
callback and calls `storage_register_fsprovider`. cardfs is an *exploration* of the
object/relational-filesystem direction: a flat key→object map (superblock + a
128-byte-entry object table + bump-allocated data blocks) used to exercise the
on-disk persistence path end-to-end. The destructive format/put/get self-test is
compiled out unless `CARDFS_SELFTEST` is set; the probe never formats a device
implicitly. The symbol's actual C name is `module_init`.

---

## `cardfs_probe`

- **kind:** function
- **lang:** c
- **source:** `drivers/cardfs/src/main.c`
- **hash:** a8dded5b320c258e

CoreStorage filesystem-provider probe: reads the superblock and mounts the device
if it carries a valid cardfs volume, otherwise declines without modifying it.

**Parameters**
- `bdev` — the CoreStorage block device offered by the registry.

**Returns** 0 if the device has a valid `CARDFS01` superblock (mounted), -1
otherwise (left untouched). When built with `CARDFS_SELFTEST`, a non-cardfs device
is instead **destructively** formatted and exercised. This is the only public-ish
cardfs symbol beyond `module_init`; the on-disk helpers (`read_super`,
`cardfs_format`, `cardfs_put`, `cardfs_get`, `blk_read`/`blk_write`) are `static`
internals and are not documented here.

---
