# Your first REPL session

Drive the live Cardinal; OS from an interactive Lisp shell over the COM1 serial link.

## Prerequisites

You have already built the OS and booted it in QEMU per the [Build & boot](build-and-boot.md)
tutorial.  You will now build a second ISO — the **REPL image** — and connect to it with the
host-side terminal script.

---

## How the REPL works

The interactive Lisp shell lives in `lisp/init.clp` as `start-repl`.  When the kernel command
line contains `cardinal.repl`, `SysLisp` calls `(start-repl)` after `(system-init)` returns.
`start-repl` spawns an **unrestricted (root)** context — serial debugging is exactly the case
that wants full system authority — which:

1. Imports `sys-console` (REPL I/O primitives) and `sys-irq` (ISA interrupt lines).
2. Registers COM1's receive interrupt (ISA IRQ 4) and arms the UART RX line.
3. Imports `play-tone` and `set-vol` from `init` into the persistent REPL environment so you
   can call them bare.
4. Prints the ready banner and parks, IRQ-driven.  An arriving byte wakes the context, which
   calls `(repl-eval <input>)` — the read-eval-print engine in `libs/lisp/src/repl.c` — and
   writes the transcript back over the serial link.

**CSMUX framing.**  Once `cardinal.repl` is active, the kernel switches COM1 into a framed
multiplexed protocol: the kernel debug log rides channel 0, and the REPL rides channel 2.  A
raw terminal therefore sees byte-stuffed binary.  The host-side script `scripts/csmux-repl.py`
demultiplexes the stream, prints the log to your terminal, and frames whatever you type onto
channel 2.

**Persistent environment.**  `repl-eval` accumulates `define`s across calls (bindings are
stored in the system heap so they survive the REPL context's own garbage collection).  Every
expression you type enriches the same live namespace.

---

## Step 1 — Build the REPL image

`./scripts/build.sh` must have already run at least once (it builds all modules).  Then build
the dedicated REPL ISO, which uses `platform/x86_64/pc/grub_repl.cfg` (it passes
`cardinal.repl` on the multiboot2 kernel line):

```bash
cmake --build build --target repl-image
# -> build/ISO/os-repl.iso
```

---

## Step 2 — Launch the REPL terminal

`scripts/csmux-repl.py` boots the REPL ISO under QEMU, connects over a UNIX-socket serial
link, and gives you a line-oriented terminal:

```bash
python3 scripts/csmux-repl.py
```

The script defaults to `build/ISO/os-repl.iso`, `q35` machine, KVM acceleration (falls back
to TCG), 512 MiB RAM, and 2 vCPUs.  Override with env vars matching the options in
`run-qemu.sh`:

```bash
MEM=256 SMP=1 ACCEL=tcg python3 scripts/csmux-repl.py
```

You will see the QEMU launch line on stderr, followed by the unframed pre-CSMUX boot log as
the OS initialises.  Once `start-repl` finishes arming the IRQ you will see:

```
[repl] serial REPL ready on COM1 -- try (play-tone)
```

At that point the terminal is live.  There is no prompt character — type a Lisp expression,
press Enter, and the transcript comes back.  Press Ctrl-C to quit.

!!! note "Real hardware"
    On a physical machine, pass `--serial-device /dev/ttyUSB0` (or whichever adapter you
    have).  The script handles the same CSMUX framing over the tty.

---

## Step 3 — Evaluating expressions

The REPL evaluates every complete S-expression in the chunk it receives, reports each result
on its own line, and returns reader-faithful output (see the [VM reference](../vm/api.md) for
the value types and reader syntax).  Reader errors are reported with their line and column.

### Arithmetic

```scheme
(+ 1 2)
```
```
3
```

```scheme
(* 6 7)
```
```
42
```

```scheme
(quotient 17 5)
```
```
3
```

You can send multiple expressions on one line or across lines — the REPL evaluates all forms
in each received chunk:

```scheme
(+ 10 20) (* 3 4)
```
```
30
12
```

### Defining names

`define` binds into the persistent REPL environment and survives indefinitely:

```scheme
(define greeting "hello from the running OS")
```
```
#<undef>
```

```scheme
greeting
```
```
"hello from the running OS"
```

```scheme
(define (square x) (* x x))
(square 9)
```
```
#<undef>
81
```

### Lists and recursion

```scheme
(define (range n)
  (let loop ((i 0) (acc '()))
    (if (= i n) (reverse acc)
        (loop (+ i 1) (cons i acc)))))
(range 5)
```
```
(0 1 2 3 4)
```

### Checking capabilities

Because `start-repl` uses `(spawn ...)` (not `spawn-restricted`), the REPL context is
unrestricted:

```scheme
(capabilities)
```
```
#t
```

`#t` means unrestricted root — every `sys-*` module is importable.  See
[Modules and capabilities](../vm/api.md#5-modules-and-capabilities) for how restricted
contexts differ.

---

## Step 4 — Inspecting the live system with sys-debug

`sys-debug` is a gated capability module (`libs/lisp/src/debug.c`).  Because the REPL context
is root, you can import it at any time:

```scheme
(import sys-debug)
```
```
#<undef>
```

This binds `ctx-list`, `ctx-blocked?`, `ctx-pause`, `ctx-unpause`, `ctx-make`, `ctx-step`,
`ctx-status`, `ctx-control`, `ctx-value`, and `ctx-error` into the REPL environment.  See the
[sys-debug reference](../vm/api.md#sys-debug-reflective-debugger-capability) for the full
signature table.

### Listing live contexts

```scheme
(ctx-list)
```

`(ctx-list)` returns the run queue of **this core** (the BSP, where the REPL lives).  On a
2-vCPU boot the AP runs its own queue; `ctx-list` does not see it.  The return value is a
list of context handles — opaque objects that print as `#<ctx:N>`.  The illustrative output
below reflects a quiet boot with a few idle service contexts:

```
(#<ctx:1> #<ctx:2> #<ctx:3> #<ctx:4>)
```

The number of contexts you see depends on which services `system-init` started and how many
are sleeping vs. actively polling on the BSP's queue.

### Checking whether a context is blocked

```scheme
(let ((cs (ctx-list)))
  (map (lambda (c) (list c (ctx-blocked? c)))
       cs))
```

`(ctx-blocked? c)` is `#t` when the context is parked waiting for a message (or sleeping).
Most service contexts spend the overwhelming majority of time blocked on `recv`.  A context
that is `#f` on every poll is busy-spinning — a useful diagnostic.

```
((#<ctx:1> #t) (#<ctx:2> #t) (#<ctx:3> #f) (#<ctx:4> #t))
```
*(illustrative — your counts will differ)*

### Cooperative pause and single-step

`ctx-pause` marks a scheduler-owned context blocked, handing it exclusively to the debugger.
On a cooperative single-core scheduler, only one context runs at a time, so the target is
provably not running when you call this — it is safe to `ctx-step` a paused context.

!!! warning "SMP and stepping"
    `ctx-pause` is safe for contexts on the **BSP** queue (the REPL's core).  Stepping a
    context on an AP's queue is not safe — the AP continues running independently.  Use
    `ctx-make` (below) for a portable, always-safe stepping workflow.

```scheme
; Pick the first context from the list (adjust the index for a real target).
(define target (car (ctx-list)))
(ctx-pause target)
```

Now step it one reduction:

```scheme
(ctx-step target)
```
```
suspended
```

`suspended` means the budget ran out but the context is still runnable — step again to
continue.  `done` means the context finished.  `error` means it faulted; read the message
with `(ctx-error target)`.

Peek at what it is about to evaluate (only meaningful in `eval` state):

```scheme
(ctx-status target)
```
```
eval
```

```scheme
(ctx-control target)
```
```
(%mailbox-pop)
```

Return the context to the scheduler when you are done:

```scheme
(ctx-unpause target)
```

### Creating and stepping a context you own

For unconditionally safe stepping — no race risk, works under SMP — use `ctx-make` to create
a context that is **never** on the scheduler queue:

```scheme
(define c (ctx-make (lambda ()
  (let loop ((n 0))
    (if (= n 3) 'done
        (loop (+ n 1)))))))

(ctx-step c 100)   ; run up to 100 reductions
```
```
done
```

```scheme
(ctx-value c)
```
```
done
```

---

## Step 5 — Sending a message to a running server

The REPL context is root and carries its own Lisp environment, so you can `send` messages
directly to any live service handle you hold.  The most convenient handles are the ones
`start-repl` imported deliberately: `play-tone` and `set-vol`.

### play-tone

`play-tone` sends a tone request through the live `coreaudio` service to the HD Audio
controller registered as `'hda0`.  It is an end-to-end exercise of the audio IPC path —
the exact same message a real audio client would send.

```scheme
(play-tone)          ; default 500 Hz bring-up tone
```
```
playing
```

```scheme
(play-tone 440)      ; A4 (concert pitch) at default amplitude and duration
```
```
playing
```

```scheme
(play-tone 440 8000 9600)   ; 440 Hz, amplitude 8000, 9600 frames (0.2 s at 48 kHz)
```
```
playing
```

If no HD Audio controller came up (e.g. you did not pass `AUDIO=hda` to QEMU), `play-tone`
returns `'no-audio` rather than crashing:

```scheme
(play-tone 880)
```
```
no-audio
```

!!! tip "Attaching HD Audio"
    To hear audio output from the REPL, pass the QEMU audio args via the `QEMU_EXTRA`
    environment variable and capture to a WAV file:

    ```bash
    AUDIO_WAV=/tmp/tone.wav \
    QEMU_EXTRA="-audiodev wav,id=snd0,path=/tmp/tone.wav -device ich9-intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0" \
    python3 scripts/csmux-repl.py
    ```

    After calling `(play-tone 440)`, the WAV file will contain a non-silent burst — the
    proof that the path from the REPL through `coreaudio` to the HD Audio DMA engine ran
    end to end.

### set-vol

`set-vol` adjusts an endpoint's volume on `hda0`.  The endpoint ID comes from the boot log
(look for lines like `[hdaudio] ep 0 speaker out`):

```scheme
(set-vol 0 80)    ; endpoint 0, volume 80 %
```
```
ok
```

Pass `0` to mute:

```scheme
(set-vol 0 0)
```
```
ok
```

Both `play-tone` and `set-vol` validate argument types before forwarding: a non-integer
argument returns `'bad-args` rather than killing the audio service context.

### Sending to other services

The REPL context is root and can `send` to any handle it holds.  Service handles are not
pre-bound in the REPL environment (they live in `system-init`'s lexical scope, not the
REPL's persistent env), but you can obtain them if a service exposes a discovery mechanism
— for example, by constructing a round-trip through the object registry with `sys-reg`.

For the full message protocol of each Core* service, see the individual server pages under
`docs/servers/`.

---

## Step 6 — Using --exec for non-interactive evaluation

`csmux-repl.py` supports a scripted mode: it waits for the ready banner, sends each
`--exec` expression, prints the output, and exits once the REPL goes quiet.  Useful for
one-shot probes from a CI job or a shell script:

```bash
python3 scripts/csmux-repl.py \
  --exec "(+ 1 2)" \
  --exec "(define x 99)" \
  --exec "x"
```

Expected output (interspersed with the boot log):
```
3
#<undef>
99
```

Pass `--script path/to/file.clp` to send a multi-line file of Lisp.

---

## Next steps

- [Write your first driver](first-driver.md) — bind a PCI device in `lisp/init.clp` and
  register it with a Core* service.
- [Message passing](../concepts/message-passing.md) — a deeper look at how contexts
  communicate: copy-on-send semantics, the `reply-to` guard, and the server pattern.
