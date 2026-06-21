#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# extract.py -- the single Cardinal; documentation extractor.
#
# It parses every docs/api/*.md file into entries (per docs/README.md, the format
# contract), locates each documented symbol's DEFINITION in the file named by its
# `source:` line, and computes sha256(normalize(span))[:16] -- the freshness hash.
# An entry is `current` when that recomputed hash matches the `hash:` recorded in
# the markdown, `stale` when it differs (a recorded `pending` is always stale),
# `missing-source` when the locator finds nothing, and `n/a` for `overview`
# entries (which are not hashed).
#
# Outputs (always, under build/docs/):
#   db.json          -- machine-readable database for gen_site.py
#   ../../lisp/docs-db.clp  -- a generated Lisp module the OS imports for `man`
# With --update, it also rewrites each entry's `hash:` line in place to the freshly
# computed value. With --check, it exits non-zero if anything is stale/missing.

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DOCS_API_DIR = os.path.join(REPO_ROOT, "docs", "api")
BUILD_DOCS_DIR = os.path.join(REPO_ROOT, "build", "docs")
DOCS_DB_CLP = os.path.join(REPO_ROOT, "lisp", "docs-db.clp")

# Lisp body strings are read by a length-bounded reader (buf[1024] in
# libs/lisp/src/reader.c), so a single string literal longer than that fails to
# parse. Truncate bodies well under that, leaving headroom for escape expansion.
BODY_MAX = 900
# brief shares the same length-bounded reader buffer; cap it identically.
BRIEF_MAX = 900
# name/source never legitimately approach the buf[1024] limit; this cap is a
# defensive safety floor so a pathological value can't overflow the literal.
FIELD_MAX = 512
ELLIPSIS = " [...]"

# The Lisp-module emitter writes kind/lang/status as BARE symbols (the reader
# interns them). A value containing whitespace/parens/quotes/semicolons would
# emit structurally invalid Lisp and break boot (docs-db.clp is imported
# unconditionally before the heap freezes). Restrict each to a fixed allowlist
# and abort extraction on any violation rather than emit a broken module.
VALID_KINDS = {
    "function", "macro", "struct", "typedef", "enum", "constant",
    "lisp-fn", "lisp-const", "overview",
}
VALID_LANGS = {"c", "lisp"}
VALID_STATUSES = {"current", "stale", "missing-source", "n/a"}

# A heading line: ## `name` (an H2 whose only content is a backticked symbol).
H2_RE = re.compile(r"^##\s+`([^`]+)`\s*$")
# A metadata bullet: - **key:** value
META_RE = re.compile(r"^-\s+\*\*([a-z]+):\*\*\s*(.*?)\s*$")


# ---------------------------------------------------------------------------
# Markdown entry parsing
# ---------------------------------------------------------------------------

def parse_doc_file(path):
    """Parse one docs/api/*.md into a list of entry dicts.

    Each entry: name, kind, lang, source, recorded_hash, brief, body, and
    line-span bookkeeping (heading_line, hash_line) used by --update.
    """
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")

    entries = []
    i = 0
    n = len(lines)
    while i < n:
        m = H2_RE.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1).strip()
        heading_line = i
        i += 1

        # Metadata: the contiguous run of `- **key:** value` bullets (allowing
        # blank lines between the heading and the list, and inside it).
        meta = {}
        hash_line = None
        while i < n:
            line = lines[i]
            if line.strip() == "":
                i += 1
                continue
            mm = META_RE.match(line)
            if not mm:
                break
            key, val = mm.group(1), mm.group(2)
            # `source:`/`hash:` values may be wrapped in backticks; unwrap.
            val = val.strip()
            if val.startswith("`") and val.endswith("`") and len(val) >= 2:
                val = val[1:-1]
            meta[key] = val
            if key == "hash":
                hash_line = i
            i += 1

        # Brief: the first non-empty paragraph after the metadata.
        while i < n and lines[i].strip() == "":
            i += 1
        brief_lines = []
        while i < n and lines[i].strip() != "":
            brief_lines.append(lines[i])
            i += 1
        brief = " ".join(l.strip() for l in brief_lines).strip()

        # Body: everything up to the next `## ` heading or EOF. A trailing `---`
        # separator is cosmetic; strip it from the captured body.
        body_lines = []
        while i < n and not lines[i].startswith("## "):
            body_lines.append(lines[i])
            i += 1
        body = "\n".join(body_lines).strip()
        body = re.sub(r"\n?-{3,}\s*$", "", body).strip()

        entries.append({
            "name": name,
            "kind": meta.get("kind", "").strip(),
            "lang": meta.get("lang", "").strip(),
            "source": meta.get("source", "").strip(),
            "recorded_hash": meta.get("hash", "").strip(),
            "brief": brief,
            "body": body,
            "heading_line": heading_line,
            "hash_line": hash_line,
        })
    return entries, lines


# ---------------------------------------------------------------------------
# Normalization
# ---------------------------------------------------------------------------

def _strip_c_comments(text):
    """Remove C /* */ and // comments, preserving string/char literal contents."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _strip_lisp_comments(text):
    """Remove Lisp ; line comments, preserving string literal contents."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == ";":
            while i < n and text[i] != "\n":
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def normalize(span, lang):
    """Strip comments for the language, collapse whitespace runs, trim."""
    if lang == "lisp":
        span = _strip_lisp_comments(span)
    else:
        span = _strip_c_comments(span)
    span = re.sub(r"\s+", " ", span)
    return span.strip()


# ---------------------------------------------------------------------------
# Locators -- find a definition's text span in its source file
# ---------------------------------------------------------------------------

def _match_braces(text, open_idx):
    """Given the index of a `{`, return the index just past its matching `}`,
    honoring C string/char literals and comments. None if unbalanced."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def _match_parens_c(text, open_idx):
    """C analogue of _match_parens: index just past the `)` matching the `(` at
    open_idx, honoring C string AND char literals and // and /* */ comments. The
    Lisp _match_parens treats `;` as a comment and ignores char literals, so it
    must not be used on C text. None if unbalanced."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def _next_significant_c(text, start):
    """Index of the next non-whitespace, non-comment character at or after
    `start`, skipping C // and /* */ comments. None if none remains. (Stops at
    the first real token, e.g. the '{' of a definition or ';' of a prototype.)"""
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        return i
    return None


def locate_c_function(text, name):
    """Find `... name(args) { ... }` and brace-match to the closing }."""
    # name followed by ( -- the call/def site. Require a non-identifier char
    # before the name so e.g. `foo` does not match `barfoo`.
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(", text):
        # Paren-match the parameter list with the C-aware matcher so a ')' inside
        # a parameter comment or string literal does not end it early. The regex
        # ends on the opening '(', so anchor on m.end()-1.
        open_idx = m.end() - 1
        j = _match_parens_c(text, open_idx)
        if j is None:
            continue
        # Skip forward to the next significant char (honoring comments/literals);
        # it must be '{' for this to be a definition (a prototype ends in ';').
        k = _next_significant_c(text, j)
        if k is not None and k < len(text) and text[k] == "{":
            # span starts at the line containing the name (its return type etc.);
            # find the start of the declaration line.
            start = text.rfind("\n", 0, m.start()) + 1
            end = _match_braces(text, k)
            if end is not None:
                return text[start:end]
    return None


def locate_c_macro(text, name):
    """`#define name ...` plus backslash-continued lines."""
    pat = re.compile(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"\b", re.M)
    m = pat.search(text)
    if not m:
        return None
    start = m.start()
    i = m.end()
    n = len(text)
    # Consume to end of logical line, following backslash continuations.
    while i < n:
        if text[i] == "\n":
            # A line continuation is a backslash immediately before this newline.
            j = i - 1
            if j >= start and text[j] == "\\":
                i += 1
                continue
            break
        i += 1
    return text[start:i]


def locate_c_record(text, name):
    """struct/enum/union body brace-matched, or a `typedef ... name;` line."""
    # struct|enum|union <name> { ... }. There may be several occurrences of
    # `struct NAME` -- a forward declaration (`struct NAME;`) or a typedef
    # reference (`typedef struct NAME NAME_t;`) precede the real body. Iterate
    # every occurrence and skip any whose next significant token is not `{`,
    # so we land on the actual `struct NAME { ... }` body to brace-match.
    for kw in ("struct", "enum", "union"):
        for m in re.finditer(r"\b" + kw + r"\s+" + re.escape(name) + r"\b", text):
            brace = text.find("{", m.end())
            semi = text.find(";", m.end())
            if brace != -1 and (semi == -1 or brace < semi):
                end = _match_braces(text, brace)
                if end is not None:
                    start = text.rfind("\n", 0, m.start()) + 1
                    # include up to the terminating ';' after the '}'
                    tail = text.find(";", end)
                    if tail != -1 and text[end:tail].strip() == "":
                        end = tail + 1
                    return text[start:end]
            # (no '{' before the next ';': forward decl / typedef ref; try next.)
    # typedef struct { ... } name;  -- the name trails the body.
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*;", text):
        # Find a 'typedef' that precedes this, with a brace-matched body between.
        lstart = text.rfind("typedef", 0, m.start())
        if lstart == -1:
            continue
        brace = text.find("{", lstart)
        if brace != -1 and brace < m.start():
            end = _match_braces(text, brace)
            if end is not None and end <= m.start():
                return text[lstart:m.end()]
        else:
            # typedef <type> name;  (no brace, e.g. typedef uint32_t foo;)
            if text.find(";", lstart) == m.end() - 1:
                return text[lstart:m.end()]
    return None


def _find_statement_end(text, start):
    """Index just past the `;` that terminates the statement beginning at
    `start`, honoring C string/char literals, comments, and any nested
    parens/braces so a `;` inside them does not end the statement early."""
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == "(":
            close = _match_parens_c(text, i)
            if close is None:
                return None
            i = close
            continue
        if c == "{":
            close = _match_braces(text, i)
            if close is None:
                return None
            i = close
            continue
        if c == ";":
            return i + 1
        i += 1
    return None


def locate_c_typedef(text, name):
    """Locate a `typedef ... NAME;` statement and return its full span.

    Handles both the function-pointer form `typedef <ret> (*NAME)(<args>);`
    (which may span multiple lines, ending at the terminating `;`) and the
    simple alias form `typedef <type> NAME;`. Falls back to the brace-bearing
    struct/enum/union typedef cases handled by locate_c_record.
    """
    # Function-pointer typedef: `(*NAME)` introduced by a `typedef`.
    for m in re.finditer(r"\(\s*\*\s*" + re.escape(name) + r"\s*\)", text):
        lstart = text.rfind("typedef", 0, m.start())
        if lstart == -1:
            continue
        # Reject if a ';' sits between the typedef and the (*NAME) -- that would
        # mean this typedef belongs to a different (earlier) statement.
        if ";" in text[lstart:m.start()]:
            continue
        end = _find_statement_end(text, m.end())
        if end is not None:
            return text[lstart:end]
    # Simple alias typedef: `typedef ... NAME;` where NAME is the last
    # identifier before the terminating ';' and the statement has no braces.
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*;", text):
        lstart = text.rfind("typedef", 0, m.start())
        if lstart == -1:
            continue
        between = text[lstart:m.start()]
        if ";" in between or "{" in between:
            continue
        return text[lstart:m.end()]
    # Brace-bearing typedef forms (typedef struct {...} NAME; etc.).
    return locate_c_record(text, name)


def locate_c_enum_member(text, name):
    """Locate NAME as an enum member and return the ENCLOSING `enum [tag] { ... }`
    block, brace-matched. The whole enum is the staleness unit for an enum
    member: a member's value is implicit in the order/initializers of its
    siblings, so any change to the enum body is what we want to flag."""
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", text):
        # Find the innermost enclosing brace pair whose `enum` keyword opens it.
        # Walk back over balanced braces to the open `{` that contains us.
        open_brace = _enclosing_open_brace(text, m.start())
        if open_brace is None:
            continue
        # The token right before this `{` (skipping a possible tag identifier)
        # must be `enum` for NAME to be an enum member.
        head = text[:open_brace]
        hm = re.search(r"\benum\b\s*(?:[A-Za-z_]\w*\s*)?$", head)
        if not hm:
            continue
        end = _match_braces(text, open_brace)
        if end is None:
            continue
        start = text.rfind("\n", 0, hm.start()) + 1
        tail = text.find(";", end)
        if tail != -1 and text[end:tail].strip() == "":
            end = tail + 1
        return text[start:end]
    return None


def _enclosing_open_brace(text, pos):
    """Return the index of the innermost `{` that encloses position `pos`,
    honoring literals/comments. None if `pos` is not inside any braces."""
    stack = []
    i = 0
    n = len(text)
    # Strictly `< pos`: a `{` at `pos` itself encloses nothing, so it must not be
    # counted as the enclosing brace.
    while i < n and i < pos:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == "{":
            stack.append(i)
        elif c == "}":
            if stack:
                stack.pop()
        i += 1
    return stack[-1] if stack else None


def locate_c_prototype(text, name):
    """Locate a function PROTOTYPE declaration `... NAME(...);` and return the
    full statement span. Used as a fallback when a `kind: function` body is not
    found in `source:` -- e.g. functions declared only in a header whose
    implementation is out-of-tree. The prototype is then the best available
    staleness signal: a change to the declared signature flips the hash."""
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(", text):
        # The regex ends on the opening '(' of the parameter list; anchor on it
        # directly (m.end()-1) rather than re-scanning, which could land on a '('
        # inside a preceding comment. Then require the next significant token to
        # be ';' (a declaration), not '{' (a definition).
        open_idx = m.end() - 1
        close = _match_parens_c(text, open_idx)
        if close is None:
            continue
        k = close
        while k < len(text) and text[k] in " \t\r\n":
            k += 1
        if k < len(text) and text[k] == ";":
            start = text.rfind("\n", 0, m.start()) + 1
            return text[start:k + 1]
    return None


def _match_parens(text, open_idx):
    """Index of the index just past the `)` matching the `(` at open_idx,
    honoring Lisp string literals and ; comments. None if unbalanced."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == ";":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def locate_lisp(text, name):
    """`(define (name ...) ...)` or `(define name ...)`, paren-matched."""
    # Function form: (define (name ...)
    pat_fn = re.compile(r"\(\s*define\s*\(\s*" + re.escape(name) + r"[\s)]")
    pat_val = re.compile(r"\(\s*define\s+" + re.escape(name) + r"[\s)]")
    for pat in (pat_fn, pat_val):
        m = pat.search(text)
        if m:
            # The '(' that opens the define form is the first '(' in the match.
            open_idx = text.index("(", m.start())
            end = _match_parens(text, open_idx)
            if end is not None:
                return text[open_idx:end]
    return None


def locate(entry):
    """Dispatch to the right locator; return the raw span text or None."""
    src = entry["source"]
    if not src:
        return None
    abspath = os.path.join(REPO_ROOT, src)
    if not os.path.isfile(abspath):
        return None
    with open(abspath, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    name = entry["name"]
    kind = entry["kind"]
    lang = entry["lang"]
    if lang == "lisp" or kind in ("lisp-fn", "lisp-const"):
        return locate_lisp(text, name)
    if kind == "function":
        # Prefer the definition body; fall back to the prototype declaration for
        # header-only-declared functions (implementation out-of-tree).
        return locate_c_function(text, name) or locate_c_prototype(text, name)
    if kind == "macro":
        return locate_c_macro(text, name)
    if kind == "typedef":
        return locate_c_typedef(text, name)
    if kind in ("struct", "enum"):
        return locate_c_record(text, name)
    if kind == "constant":
        # A constant may be an object-like #define, a typedef-style value, or an
        # enum member; try #define, then the enum-member case (hash the enum).
        return (locate_c_macro(text, name)
                or locate_c_record(text, name)
                or locate_c_enum_member(text, name))
    return None


# ---------------------------------------------------------------------------
# Status + per-entry computation
# ---------------------------------------------------------------------------

def compute_entry(entry, docfile_rel):
    """Fill computed_hash / status; return a result dict for db.json."""
    kind = entry["kind"]
    if kind == "overview":
        status = "n/a"
        computed = None
    else:
        span = locate(entry)
        if span is None:
            status = "missing-source"
            computed = None
        else:
            computed = hashlib.sha256(
                normalize(span, entry["lang"]).encode("utf-8")).hexdigest()[:16]
            status = "current" if computed == entry["recorded_hash"] else "stale"
    return {
        "name": entry["name"],
        "kind": kind,
        "lang": entry["lang"],
        "source": entry["source"],
        "brief": entry["brief"],
        "body": entry["body"],
        "recorded_hash": entry["recorded_hash"],
        "computed_hash": computed,
        "status": status,
        "docfile": docfile_rel,
    }


# ---------------------------------------------------------------------------
# Lisp module emission
# ---------------------------------------------------------------------------

def lisp_escape(s):
    """Escape a Python string for the Cardinal Lisp reader (handles ", \\,
    and turns newlines/tabs/CR into their escape sequences)."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    return "".join(out)


def truncate(s, limit):
    """Truncate a string to `limit` chars (pre-escape) with an ellipsis marker."""
    if len(s) <= limit:
        return s
    return s[:limit].rstrip() + ELLIPSIS


def truncate_body(body):
    return truncate(body, BODY_MAX)


def emit_lisp_module(results, path):
    """Write lisp/docs-db.clp -- a (define-module docs-db (export doc-entries) ...).

    Each entry is a 7-element list:
      (name-string kind-symbol lang-symbol source-string status-symbol
       brief-string body-string)
    Symbols (kind/lang/status) are written bare so the reader interns them; the
    four free-text fields are escaped string literals.
    """
    # Validate every entry's bare-symbol fields BEFORE emitting anything. A bad
    # value would produce invalid Lisp and break boot, so abort with a clear
    # error rather than write a broken docs-db.clp.
    for r in results:
        for field, allowed in (("kind", VALID_KINDS),
                               ("lang", VALID_LANGS),
                               ("status", VALID_STATUSES)):
            val = r[field]
            if val not in allowed:
                print(
                    "extract.py: ERROR: invalid %s %r for entry %r (in %s); "
                    "expected one of %s -- refusing to emit docs-db.clp"
                    % (field, val, r["name"], r["docfile"],
                       sorted(allowed)),
                    file=sys.stderr)
                sys.exit(1)

    lines = []
    lines.append(";; docs-db: GENERATED by scripts/docs/extract.py -- do not edit.")
    lines.append(";; The machine half of the Cardinal; doc system: one quoted list of")
    lines.append(";; documented symbols, imported by lisp/manpage.clp to serve `man`/")
    lines.append(";; `apropos` over the serial REPL. Regenerated by the build, git-ignored.")
    lines.append("")
    lines.append("(define-module docs-db")
    lines.append("  (export doc-entries)")
    lines.append("  (define doc-entries")
    lines.append("    (quote (")
    for r in results:
        kind = r["kind"]
        lang = r["lang"]
        status = r["status"]
        name = lisp_escape(truncate(r["name"], FIELD_MAX))
        source = lisp_escape(truncate(r["source"], FIELD_MAX))
        brief = lisp_escape(truncate(r["brief"], BRIEF_MAX))
        body = lisp_escape(truncate_body(r["body"]))
        lines.append(
            '      ("%s" %s %s "%s" %s "%s" "%s")'
            % (name, kind, lang, source, status, brief, body))
    lines.append("    ))))")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------------------
# --update: rewrite hash lines in place
# ---------------------------------------------------------------------------

def apply_updates(docfile, entries, results):
    """Rewrite each entry's `hash:` line to its freshly computed value.

    Skips overview (no hash) and missing-source (nothing to stamp). Re-reads the
    file, edits the recorded hash_line indices captured at parse time.
    """
    with open(docfile, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    changed = 0
    for entry, res in zip(entries, results):
        if res["status"] in ("n/a", "missing-source"):
            continue
        if res["computed_hash"] is None:
            continue
        hl = entry["hash_line"]
        if hl is None:
            continue
        old = lines[hl]
        new = re.sub(r"(\*\*hash:\*\*\s*).*$", r"\g<1>" + res["computed_hash"], old)
        if new != old:
            lines[hl] = new
            changed += 1
    if changed:
        with open(docfile, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
    return changed


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def git_short_sha():
    try:
        out = subprocess.check_output(
            ["git", "-C", REPO_ROOT, "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL)
        return out.decode("utf-8").strip()
    except Exception:
        return "unknown"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Cardinal; documentation extractor: parse docs/api/*.md, "
                    "hash the documented source spans, and emit build/docs/db.json "
                    "+ lisp/docs-db.clp.")
    ap.add_argument("--update", action="store_true",
                    help="rewrite each entry's hash: line in its .md to the freshly "
                         "computed value (re-stamp after re-verifying the code).")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any entry is stale or missing-source "
                         "(for CI freshness gating).")
    args = ap.parse_args(argv)

    if not os.path.isdir(DOCS_API_DIR):
        print("docs/api/ not found at %s (no docs to extract)" % DOCS_API_DIR)
        # Still emit empty artifacts so downstream steps have something to read.
        md_files = []
    else:
        md_files = sorted(
            os.path.join(DOCS_API_DIR, f)
            for f in os.listdir(DOCS_API_DIR)
            if f.endswith(".md"))

    all_results = []
    for docfile in md_files:
        docfile_rel = os.path.relpath(docfile, REPO_ROOT)
        entries, _lines = parse_doc_file(docfile)
        results = [compute_entry(e, docfile_rel) for e in entries]
        if args.update:
            n = apply_updates(docfile, entries, results)
            if n:
                # Re-status the updated entries as current (their recorded hash now
                # equals the computed one).
                for r in results:
                    if r["status"] == "stale" and r["computed_hash"] is not None:
                        r["recorded_hash"] = r["computed_hash"]
                        r["status"] = "current"
        all_results.extend(results)

    # Emit artifacts.
    os.makedirs(BUILD_DOCS_DIR, exist_ok=True)
    db = {
        "generated_from": git_short_sha(),
        "entries": [
            {k: r[k] for k in ("name", "kind", "lang", "source", "brief", "body",
                               "recorded_hash", "computed_hash", "status", "docfile")}
            for r in all_results
        ],
    }
    with open(os.path.join(BUILD_DOCS_DIR, "db.json"), "w", encoding="utf-8") as f:
        json.dump(db, f, indent=2)
    emit_lisp_module(all_results, DOCS_DB_CLP)

    # Summary.
    counts = {"current": 0, "stale": 0, "missing-source": 0, "n/a": 0}
    for r in all_results:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print("docs: %d entries from %d file(s)" % (len(all_results), len(md_files)))
    print("  current=%d  stale=%d  missing-source=%d  overview(n/a)=%d"
          % (counts["current"], counts["stale"], counts["missing-source"],
             counts["n/a"]))
    print("  wrote %s" % os.path.relpath(os.path.join(BUILD_DOCS_DIR, "db.json"), REPO_ROOT))
    print("  wrote %s" % os.path.relpath(DOCS_DB_CLP, REPO_ROOT))

    if args.check and (counts["stale"] or counts["missing-source"]):
        print("docs: FAILED --check (stale or missing-source entries present)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
