# Cardinal; documentation system

This tree holds the **source of truth** for Cardinal;'s API documentation. It is
written as plain markdown, one file per subsystem under `docs/api/`, and is
consumed three ways from a single extractor:

1. **In the OS** — `scripts/docs/extract.py` emits a generated Lisp module
   `lisp/docs-db.clp` (git-ignored) that is packaged into the initrd. The
   hand-written `lisp/manpage.clp` module `(import docs-db)`s it and serves
   `(man 'symbol)` / `(apropos "text")` over the COM1 serial REPL.
2. **On the web** — `scripts/docs/gen_site.py` renders the same data to a static
   site under `_site/`, published to GitHub Pages by `.github/workflows/docs.yml`.
3. **As a freshness gate** — every documented symbol records a hash of the
   normalized source it describes. The extractor recomputes that hash from the
   *current* source and flags any entry whose code has drifted as **STALE**, both
   in the OS (`man` prints a badge) and on the web.

## Why a per-symbol hash

Docs rot silently. Tying each doc entry to a `sha256` of the *normalized body* of
the code it documents means any change to that function/macro/struct/Lisp
definition flips the hash, so the system can mark the doc stale until a human
re-reads the code and re-stamps it. The hash is computed **only** by the Python
extractor; the OS and the website just display the precomputed status, so there
is no hashing logic to keep in sync across languages.

## Entry format (the contract)

Each `docs/api/<subsystem>.md` file is a sequence of **entries**. An entry is:

```markdown
## `symbol_name`

- **kind:** function
- **lang:** c
- **source:** `relative/path/from/repo/root.c`
- **hash:** pending

One-sentence brief, as its own paragraph immediately after the metadata.

Free-form markdown body: parameters, return value, notes, examples, caveats.
Use a `**Parameters**` / `**Returns**` convention where it helps.

---
```

Hard rules the extractor depends on (`scripts/docs/extract.py`):

- An entry **starts** at a line matching `` ^## `<name>` `` (an H2 whose only
  content is the symbol name in backticks). Nothing else uses H2.
- The four metadata lines form a markdown bullet list of `- **key:** value`
  directly under the heading, before the brief. Keys, all lowercase:
  - `kind` — one of `function`, `macro`, `struct`, `typedef`, `enum`,
    `constant`, `lisp-fn`, `lisp-const`, `overview`. Informational + used to
    pick the locator.
  - `lang` — `c` or `lisp`.
  - `source` — repo-root-relative path to the file containing the **definition**
    whose body is hashed. For a C function this is the `.c` with the body (not
    the prototype header); for a macro/struct/typedef/enum it is the header that
    defines it; for a Lisp symbol the `.clp` with the `(define ...)`. Wrap the
    path in backticks.
  - `hash` — leave as `pending` when writing; `extract.py --update` stamps the
    real value. Never hand-edit to a real hash.
- The **brief** is the first non-empty paragraph after the metadata list. Keep it
  to one sentence; it is what `apropos` and the site index show.
- The **body** is everything from after the brief up to the next `## ` or EOF.
- A trailing `---` between entries is optional (cosmetic).

### The `overview` kind

For third-party or very large internal C surfaces we do **not** document every
function (e.g. `libs/miniz`, the `libs/lisp` C VM API, `libs/png`). Write a
single `overview` entry per such unit: `kind: overview`, `source:` pointing at
the main header, brief + body describing the unit and pointing at upstream docs.
`overview` entries are **not** hashed (status is always `n/a`).

## Hashing & normalization (reference — implemented in extract.py)

Given `(lang, source, name)` the extractor locates the definition's text span and
computes `sha256(normalize(span))`, recording the first 16 hex chars.

- **C function:** find the definition `… name(…) { … }` and brace-match to the
  closing `}`.
- **C macro:** the `#define name …` line plus any `\`-continued lines.
- **C struct/enum:** `struct|enum name { … }` brace-matched (or the `typedef`).
- **Lisp:** the `(define (name …) …)` or `(define name …)` form, paren-matched.
- **normalize:** strip comments (C `/* */` and `//`; Lisp `;`), collapse all
  whitespace runs to a single space, trim ends.

Status per entry: `current` (recorded == computed), `stale` (differ, incl.
`pending`), `missing-source` (locator found nothing), `n/a` (overview).

## Workflow

```bash
# After editing docs or code, stamp the hashes of entries you've re-verified:
python3 scripts/docs/extract.py --update

# Build the machine DB (build/docs/db.json) + the OS module (lisp/docs-db.clp):
python3 scripts/docs/extract.py

# Render the website locally:
python3 scripts/docs/gen_site.py   # -> _site/index.html
```

`lisp/docs-db.clp` is generated and git-ignored; the normal `./scripts/build.sh`
+ `image` flow regenerates it so the initrd always carries docs matching the
checked-out source.
