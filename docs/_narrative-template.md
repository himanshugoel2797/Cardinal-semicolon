<!--
AUTHORING CONTRACT for narrative docs (tutorials, guides, concepts) — NOT published.

This is different from `_authoring-template.md`, which is for per-symbol API
reference. These pages are prose: tutorials (learning-oriented, end-to-end),
how-to guides (task-oriented, focused steps), and concept articles
(understanding-oriented explanation).

AUDIENCE: a developer new to Cardinal; who has the API reference open in another
tab. Assume general OS/systems literacy; do NOT assume they know this codebase.

NON-NEGOTIABLE — GROUND EVERYTHING IN SOURCE:
- Read the actual source (`.clp`, `.c`), the boot policy (`lisp/init.clp`), the
  build/run scripts (`scripts/`), `CLAUDE.md`, and the relevant `notes/` design
  docs BEFORE writing. Do not invent APIs, function names, message tags, file
  paths, CLI flags, or shell commands. If you state that a command works, it must
  be a real command from the repo. If you are unsure whether something works,
  describe it as "intended"/"per the design notes" rather than asserting it.
- The `notes/` directory holds internal design docs — mine them for the "why",
  but SYNTHESIZE into reader-facing prose; do not paste them verbatim.
- When you reference an API, LINK to its reference page instead of re-documenting
  it (see cross-linking below).

VOICE & SHAPE:
- Tutorials/guides: second person ("you"), imperative steps, a concrete goal up
  top and a "Next steps" list at the bottom linking onward.
- Concept articles: explanatory third person; lead with the mental model, then
  the mechanism, then the consequences/gotchas. End with "See also" links.
- Keep it tight. Show real code/commands. Prefer a working example over prose.

FORMATTING (MkDocs Material):
- Start with `# Title` then a one-sentence summary line (italic or plain).
- Code fences: ```bash for shell, ```scheme for Lisp, ```c for C.
- Use admonitions for asides:
  !!! note / !!! tip / !!! warning "title"
      indented body
- Cross-link with RELATIVE paths from your file's directory. From a page in
  `docs/guides/` or `docs/concepts/` or `docs/tutorials/`:
    - a server page:  `../servers/corepower.md`
    - a driver page:  `../drivers/rtl8139.md`
    - the VM reference: `../vm/index.md`
    - a sibling guide: `other-guide.md`
    - a guide from a tutorial: `../guides/add-a-pci-driver.md`
- Refer to source files as inline code with repo-relative paths, e.g.
  `lisp/servers/corepower.clp` — readers map these to the repo themselves.

ACCURACY CHECKS specific to this codebase (get these right):
- The Lisp VM sandbox IS userspace — a capability-gated context, NOT ring-3. Never
  call it ring-3 or imply a hardware privilege boundary.
- Drivers/servers communicate by message passing (`send`/`recv`), not C ABIs.
- `lisp/init.clp` is the sole device binder (gated on `pci-find`); there is no C
  driver binder / devices.txt anymore.
- Build is Clang/LLVM cross-compiling to `x86_64-elf`; two CMake builds (host
  tools vs target). Boot scripts are CRLF.
- Capabilities are granted per context via `import` of `sys-*` modules.
-->

# <Title>

*One-sentence summary of what the reader will learn or accomplish.*

<!-- body per the contract above -->

## Next steps  <!-- tutorials/guides -->
- [Related guide](other.md)

## See also  <!-- concept articles -->
- [Reference page](../servers/foo.md)
