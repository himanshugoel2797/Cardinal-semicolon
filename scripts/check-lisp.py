#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Host-side static syntax checker for Cardinal; Lisp (.clp) source.
#
# It is a faithful port of the kernel's s-expression reader
# (libs/lisp/src/reader.c): same delimiter rules, same #-syntax, same comment
# and string handling, and the same error messages and error-location policy
# (an unterminated list/string points back at its opening '(' / '"'). Because it
# mirrors the reader the VM actually runs, a file that passes here will lex/parse
# the same way at boot -- the point is to catch paren imbalance, stray ')',
# unterminated strings, '[' '{' misuse, and bad #-literals on the host, before a
# QEMU boot, with a precise file:line:col and a caret.
#
# This is a SYNTAX (reader) check only: it does not expand macros, resolve
# imports, or evaluate. A balanced, well-formed file can still be semantically
# wrong -- but it will not panic the reader.
#
#   scripts/check-lisp.py                 # check every lisp/**/*.clp
#   scripts/check-lisp.py a.clp b.clp     # check specific files
#   scripts/check-lisp.py -               # check stdin (handy for a snippet)
#   scripts/check-lisp.py -q lisp/...     # quiet: only print failures
#
# Exit status is 0 iff every checked input parses cleanly to EOF.

import os
import sys
import glob


class ReadError(Exception):
    """A reader error pinned to a byte offset, mirroring reader.c's *err/errpos."""

    def __init__(self, pos, msg):
        super().__init__(msg)
        self.pos = pos
        self.msg = msg


# --- character classes (reader.c is_ws / is_delim) --------------------------

def is_ws(c):
    return c in " \t\n\r\f"


def is_delim(c):
    # A delimiter ends an atom token. ` and , are reader macros, so they delimit.
    return is_ws(c) or c in "()[]{}\";'`,"


class Reader:
    """Port of reader.c. `read_datum` returns a non-None sentinel on success,
    the EOF sentinel at end of input, and raises ReadError on a parse error."""

    EOF = object()
    OK = object()  # stands in for "some datum" -- we only care about well-formedness

    def __init__(self, text):
        self.s = text
        self.n = len(text)
        self.cur = 0

    # skip_ws: whitespace, and ';' comments to end of line.
    def skip_ws(self):
        s, n = self.s, self.n
        c = self.cur
        while c < n:
            ch = s[c]
            if is_ws(ch):
                c += 1
            elif ch == ';':
                while c < n and s[c] != '\n':
                    c += 1
            else:
                break
        self.cur = c

    def read_string(self):
        s, n = self.s, self.n
        open_pos = self.cur  # the opening quote
        c = self.cur + 1
        while c < n and s[c] != '"':
            ch = s[c]
            c += 1
            if ch == '\\':
                if c >= n:
                    raise ReadError(open_pos, "unterminated escape in string")
                c += 1  # the escaped char is consumed verbatim (value irrelevant here)
        if c >= n:
            raise ReadError(open_pos, "unterminated string (unclosed '\"')")
        c += 1  # closing quote
        self.cur = c
        return self.OK

    def read_atom(self):
        s, n = self.s, self.n
        start = self.cur
        c = start
        while c < n and not is_delim(s[c]):
            c += 1
        self.cur = c
        if c == start:
            raise ReadError(start, "empty token")
        # int/float-vs-symbol classification is value-only -- never a syntax error.
        return self.OK

    def read_hash(self):
        s, n = self.s, self.n
        hash_pos = self.cur  # the '#'
        c = self.cur + 1
        if c >= n:
            self.cur = c
            raise ReadError(hash_pos, "dangling '#' (nothing follows)")
        if s[c] == '\\':
            # Character literal: exactly one char after #\ (so #\( is a char, not
            # an open paren). One must be present.
            if c + 1 >= n:
                self.cur = c + 1
                raise ReadError(hash_pos, "dangling character literal after '#\\'")
            self.cur = c + 2
            return self.OK
        if s[c] == '(':  # #(...) vector literal -- read_list handles balance
            self.cur = c
            self.read_list()
            return self.OK
        # Boolean / radix token: scan to a delimiter.
        start = c
        while c < n and not is_delim(s[c]):
            c += 1
        tok = s[start:c]
        if tok in ('t', 'true', 'f', 'false'):
            self.cur = c
            return self.OK
        if len(tok) >= 2 and tok[0] in 'xX':
            for i, p in enumerate(tok[1:]):
                if p not in '0123456789abcdefABCDEF':
                    self.cur = start + 1 + i
                    raise ReadError(self.cur, "bad digit in #x hex literal")
            self.cur = c
            return self.OK
        if len(tok) >= 2 and tok[0] in 'bB':
            for i, p in enumerate(tok[1:]):
                if p not in '01':
                    self.cur = start + 1 + i
                    raise ReadError(self.cur, "bad digit in #b binary literal")
            self.cur = c
            return self.OK
        self.cur = start
        raise ReadError(hash_pos, "unsupported '#' syntax")

    def read_list(self):
        s, n = self.s, self.n
        open_pos = self.cur  # the '(' -- where to point if never closed
        self.cur += 1
        seen_elem = False
        while True:
            self.skip_ws()
            if self.cur >= n:
                raise ReadError(open_pos, "unterminated list (unclosed '(')")
            ch = s[self.cur]
            if ch == ')':
                self.cur += 1
                return self.OK
            # Dotted tail: a lone '.' followed by a delimiter / EOF.
            if ch == '.' and (self.cur + 1 >= n or is_delim(s[self.cur + 1])):
                if not seen_elem:
                    raise ReadError(self.cur, "nothing before '.' in list")
                self.cur += 1  # skip the dot
                tailv = self.read_datum()
                if tailv is self.EOF:
                    raise ReadError(open_pos, "missing element after '.' in list")
                self.skip_ws()
                if self.cur >= n or s[self.cur] != ')':
                    raise ReadError(self.cur, "expected ')' after dotted tail")
                self.cur += 1
                return self.OK
            self.read_datum()
            seen_elem = True

    def read_prefixed(self, empty_msg):
        # ' ` , ,@  -- read one following datum.
        datum = self.read_datum()
        if datum is self.EOF:
            raise ReadError(self.cur, empty_msg)
        return self.OK

    def read_datum(self):
        self.skip_ws()
        if self.cur >= self.n:
            return self.EOF
        ch = self.s[self.cur]
        if ch == '(':
            return self.read_list()
        if ch == ')':
            raise ReadError(self.cur, "unexpected ')'")
        if ch == '"':
            return self.read_string()
        if ch == "'":
            self.cur += 1
            return self.read_prefixed("nothing to quote after \"'\"")
        if ch == '`':
            self.cur += 1
            return self.read_prefixed("nothing to quasiquote after '`'")
        if ch == ',':
            self.cur += 1
            empty = "nothing to unquote after ','"
            if self.cur < self.n and self.s[self.cur] == '@':
                self.cur += 1
                empty = "nothing to unquote-splice after ',@'"
            return self.read_prefixed(empty)
        if ch == '#':
            return self.read_hash()
        if ch in '[]{}':
            raise ReadError(self.cur,
                            "'[' ']' '{' '}' are not supported (use parens)")
        return self.read_atom()


def line_col(text, pos):
    """1-based line:col for a byte offset, matching lisp_source_location."""
    line, col = 1, 1
    for p in range(min(pos, len(text))):
        if text[p] == '\n':
            line += 1
            col = 1
        else:
            col += 1
    return line, col


def check_text(text, name):
    """Read datum-by-datum to EOF. Returns a list of (line, col, msg, pos);
    empty means clean. The reader stops at the first error (as the VM does)."""
    r = Reader(text)
    while True:
        try:
            v = r.read_datum()
        except ReadError as e:
            line, col = line_col(text, e.pos)
            return [(line, col, e.msg, e.pos)]
        if v is r.EOF:
            return []


def caret(text, pos):
    """The offending source line plus a caret under `pos`, for legible output."""
    start = text.rfind('\n', 0, pos) + 1
    nl = text.find('\n', pos)
    end = nl if nl != -1 else len(text)
    src = text[start:end].replace('\t', ' ')
    return "    " + src + "\n    " + " " * (pos - start) + "^"


def main(argv):
    quiet = False
    args = []
    for a in argv[1:]:
        if a in ('-q', '--quiet'):
            quiet = True
        elif a in ('-h', '--help'):
            print("usage: check-lisp.py [-q] [files... | -]\n"
                  "  no args   check every lisp/**/*.clp\n"
                  "  files     check the given .clp files\n"
                  "  -         check stdin (a snippet)\n"
                  "  -q        quiet: print only failures\n"
                  "Exit 0 iff every input parses cleanly to EOF.")
            return 0
        else:
            args.append(a)

    if args == ['-']:
        text = sys.stdin.read()
        errs = check_text(text, '<stdin>')
        for line, col, msg, pos in errs:
            print(f"<stdin>:{line}:{col}: error: {msg}")
            print(caret(text, pos))
        return 1 if errs else 0

    if args:
        files = args
    else:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        files = sorted(glob.glob(os.path.join(root, 'lisp', '**', '*.clp'),
                                 recursive=True))

    nfail = 0
    for f in files:
        try:
            with open(f, 'r', errors='replace') as fh:
                text = fh.read()
        except OSError as e:
            print(f"{f}: error: cannot read: {e}")
            nfail += 1
            continue
        errs = check_text(text, f)
        if errs:
            nfail += 1
            for line, col, msg, pos in errs:
                print(f"{f}:{line}:{col}: error: {msg}")
                print(caret(text, pos))
        elif not quiet:
            print(f"{f}: ok")

    if not quiet:
        print(f"\n{len(files) - nfail}/{len(files)} files OK")
    if nfail:
        print(f"{nfail} file(s) failed the syntax check", file=sys.stderr)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
