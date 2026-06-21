#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# gen_site.py -- render build/docs/db.json into a static HTML site under _site/.
#
# Reads the database produced by extract.py and emits a single self-contained
# index.html: entries grouped by their source markdown file, each rendered with a
# tiny built-in markdown subset (headings, bold, inline+fenced code, lists, links,
# paragraphs), STALE / missing-source badges where status warrants, a footer
# carrying the generated_from git SHA, and an inline client-side search box backed
# by _site/search.json. No external packages, no pip.

import html
import json
import os
import re
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DB_PATH = os.path.join(REPO_ROOT, "build", "docs", "db.json")
SITE_DIR = os.path.join(REPO_ROOT, "_site")


# ---------------------------------------------------------------------------
# Minimal markdown -> HTML (self-contained subset)
# ---------------------------------------------------------------------------

def _safe_href(url):
    """True if `url` is safe to emit as an href: an http/https/mailto link, or a
    '#'/relative anchor with no scheme. Anything with another scheme (e.g.
    javascript:, data:) is rejected so doc content can't inject script URLs."""
    u = url.strip()
    if u == "":
        return False
    low = u.lower()
    if low.startswith(("http://", "https://", "mailto:")):
        return True
    # A scheme is `word:` before any '/', '?', '#'. If a ':' appears before all
    # of those, it's an unknown scheme -> reject. Otherwise it's relative.
    for i, ch in enumerate(u):
        if ch == ":":
            return False
        if ch in "/?#":
            return True
    return True


def _inline(text):
    """Inline markdown: escape HTML, then apply `code`, **bold**, [link](url)."""
    # Protect inline code spans first so their contents are not bolded/linked.
    spans = []

    def stash_code(m):
        spans.append(html.escape(m.group(1)))
        return "\x00%d\x00" % (len(spans) - 1)

    text = re.sub(r"`([^`]+)`", stash_code, text)

    # links: [label](url). Stash just the <a ...> open / </a> close tags (the URL
    # is validated + escaped exactly once here), leaving the LABEL inline so the
    # subsequent whole-line html.escape and the bold/code passes still apply to
    # it. Escaping the captured URL after the line-wide escape would double-encode
    # '&' -> '&amp;amp;'. Any href whose protocol is not http/https/mailto or a
    # '#'/relative anchor is rejected so doc content can't inject a javascript:
    # (etc.) href onto the public site -- the label is then plain text.
    tag_spans = []

    def stash_link(m):
        label, url = m.group(1), m.group(2).strip()
        if _safe_href(url):
            tag_spans.append('<a href="%s">' % html.escape(url, quote=True))
            open_marker = "\x01%d\x01" % (len(tag_spans) - 1)
            tag_spans.append("</a>")
            close_marker = "\x01%d\x01" % (len(tag_spans) - 1)
            return open_marker + label + close_marker
        # Unsafe protocol: drop the href, keep the label as plain text.
        return label

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", stash_link, text)
    text = html.escape(text)
    # restore the (already-escaped) anchor tags around the now-escaped label
    text = re.sub(r"\x01(\d+)\x01", lambda m: tag_spans[int(m.group(1))], text)
    # bold: **text**
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    # restore code spans
    text = re.sub(r"\x00(\d+)\x00", lambda m: "<code>%s</code>" % spans[int(m.group(1))],
                  text)
    return text


def render_markdown(md):
    """Render the supported markdown subset of a body string into HTML."""
    lines = md.split("\n")
    out = []
    i = 0
    n = len(lines)
    in_list = False

    def close_list():
        if out and in_list:
            out.append("</ul>")

    while i < n:
        line = lines[i]

        # Fenced code block.
        if line.strip().startswith("```"):
            if in_list:
                out.append("</ul>")
                in_list = False
            i += 1
            code = []
            while i < n and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1  # skip closing fence
            out.append("<pre><code>%s</code></pre>"
                       % html.escape("\n".join(code)))
            continue

        # Blank line.
        if line.strip() == "":
            if in_list:
                out.append("</ul>")
                in_list = False
            i += 1
            continue

        # Heading (### .. #).
        hm = re.match(r"^(#{1,6})\s+(.*)$", line)
        if hm:
            if in_list:
                out.append("</ul>")
                in_list = False
            level = min(len(hm.group(1)) + 2, 6)  # demote so body H1 != page H1
            out.append("<h%d>%s</h%d>" % (level, _inline(hm.group(2)), level))
            i += 1
            continue

        # List item.
        lm = re.match(r"^\s*[-*]\s+(.*)$", line)
        if lm:
            if not in_list:
                out.append("<ul>")
                in_list = True
            out.append("<li>%s</li>" % _inline(lm.group(1)))
            i += 1
            continue

        # Paragraph: gather consecutive non-blank, non-special lines.
        if in_list:
            out.append("</ul>")
            in_list = False
        para = [line]
        i += 1
        while i < n and lines[i].strip() != "" \
                and not lines[i].strip().startswith("```") \
                and not re.match(r"^(#{1,6})\s+", lines[i]) \
                and not re.match(r"^\s*[-*]\s+", lines[i]):
            para.append(lines[i])
            i += 1
        out.append("<p>%s</p>" % _inline(" ".join(l.strip() for l in para)))

    if in_list:
        out.append("</ul>")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Page assembly
# ---------------------------------------------------------------------------

def anchor_id(docfile, name):
    base = os.path.splitext(os.path.basename(docfile))[0]
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
    return "%s__%s" % (base, safe)


def badge(status):
    if status == "stale":
        return '<span class="badge badge-stale">STALE</span>'
    if status == "missing-source":
        return '<span class="badge badge-missing">missing-source</span>'
    return ""


CSS = """
:root { --fg:#1b1f23; --muted:#586069; --bg:#fff; --accent:#0b5fff;
        --code-bg:#f6f8fa; --border:#e1e4e8; }
* { box-sizing: border-box; }
body { margin:0; font:16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",
       Helvetica,Arial,sans-serif; color:var(--fg); background:var(--bg); }
.wrap { max-width: 960px; margin: 0 auto; padding: 1.5rem 1.25rem 4rem; }
h1 { font-size: 1.8rem; margin: 0 0 .25rem; }
.sub { color: var(--muted); margin: 0 0 1.5rem; }
#search { width:100%; padding:.6rem .75rem; font-size:1rem; border:1px solid
          var(--border); border-radius:6px; margin-bottom:1.5rem; }
.group { margin-bottom: 2.5rem; }
.group > h2 { border-bottom:1px solid var(--border); padding-bottom:.3rem;
              font-size:1.3rem; }
.entry { padding:1rem 0; border-bottom:1px solid var(--border); }
.entry h3 { margin:0 0 .3rem; font-size:1.1rem; }
.entry h3 code { background:none; padding:0; font-size:1.05rem; }
.meta { color:var(--muted); font-size:.85rem; margin:0 0 .5rem; }
.meta code { font-size:.8rem; }
.brief { font-weight:500; margin:.3rem 0; }
.body { color:#24292e; }
code { background:var(--code-bg); padding:.12em .35em; border-radius:4px;
       font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
       font-size:.88em; }
pre { background:var(--code-bg); padding:.8rem 1rem; border-radius:6px;
      overflow:auto; }
pre code { background:none; padding:0; }
a { color: var(--accent); text-decoration:none; }
a:hover { text-decoration:underline; }
.badge { display:inline-block; font-size:.7rem; font-weight:700; padding:.1em .5em;
         border-radius:10px; vertical-align:middle; margin-left:.4rem;
         text-transform:uppercase; letter-spacing:.03em; }
.badge-stale { background:#fff3cd; color:#8a6d00; border:1px solid #ffe08a; }
.badge-missing { background:#ffe3e3; color:#a30000; border:1px solid #ffb3b3; }
.toc a { display:inline-block; margin:0 .6rem .3rem 0; font-size:.9rem; }
footer { margin-top:3rem; color:var(--muted); font-size:.85rem;
         border-top:1px solid var(--border); padding-top:1rem; }
.hidden { display:none !important; }
"""

SEARCH_JS = """
(function () {
  var box = document.getElementById('search');
  if (!box) return;
  var entries = Array.prototype.slice.call(document.querySelectorAll('.entry'));
  var groups = Array.prototype.slice.call(document.querySelectorAll('.group'));
  box.addEventListener('input', function () {
    var q = box.value.trim().toLowerCase();
    entries.forEach(function (el) {
      var hay = (el.getAttribute('data-name') + ' ' +
                 el.getAttribute('data-brief')).toLowerCase();
      el.classList.toggle('hidden', q !== '' && hay.indexOf(q) === -1);
    });
    groups.forEach(function (g) {
      var any = g.querySelectorAll('.entry:not(.hidden)').length > 0;
      g.classList.toggle('hidden', !any);
    });
  });
})();
"""


def build_page(db):
    entries = db.get("entries", [])
    generated_from = db.get("generated_from", "unknown")

    # Group by docfile, preserving first-seen order.
    groups = {}
    order = []
    for e in entries:
        d = e["docfile"]
        if d not in groups:
            groups[d] = []
            order.append(d)
        groups[d].append(e)

    parts = []
    parts.append("<!doctype html>")
    parts.append('<html lang="en"><head><meta charset="utf-8">')
    parts.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
    parts.append("<title>Cardinal; API documentation</title>")
    parts.append("<style>%s</style></head><body><div class=\"wrap\">" % CSS)
    parts.append("<h1>Cardinal; API documentation</h1>")
    parts.append('<p class="sub">%d documented symbols across %d subsystem file(s).</p>'
                 % (len(entries), len(order)))
    parts.append('<input id="search" type="search" placeholder="Search by name or brief...">')

    # Table of contents.
    parts.append('<nav class="toc">')
    for d in order:
        base = os.path.splitext(os.path.basename(d))[0]
        parts.append('<a href="#group__%s">%s</a>' % (re.sub(r"[^A-Za-z0-9_.-]", "_", base),
                                                      html.escape(base)))
    parts.append("</nav>")

    for d in order:
        base = os.path.splitext(os.path.basename(d))[0]
        gid = re.sub(r"[^A-Za-z0-9_.-]", "_", base)
        parts.append('<section class="group" id="group__%s">' % gid)
        parts.append("<h2>%s</h2>" % html.escape(base))
        for e in groups[d]:
            aid = anchor_id(d, e["name"])
            parts.append('<article class="entry" id="%s" data-name="%s" data-brief="%s">'
                         % (aid, html.escape(e["name"], quote=True),
                            html.escape(e["brief"], quote=True)))
            parts.append('<h3><code>%s</code>%s</h3>'
                         % (html.escape(e["name"]), badge(e["status"])))
            src = e["source"]
            meta = "%s &middot; %s" % (html.escape(e["kind"] or "?"),
                                       html.escape(e["lang"] or "?"))
            if src:
                meta += ' &middot; <code>%s</code>' % html.escape(src)
            parts.append('<p class="meta">%s</p>' % meta)
            if e["brief"]:
                parts.append('<p class="brief">%s</p>' % _inline(e["brief"]))
            if e["body"]:
                parts.append('<div class="body">%s</div>' % render_markdown(e["body"]))
            parts.append("</article>")
        parts.append("</section>")

    parts.append('<footer>Generated from <code>%s</code> by '
                 'scripts/docs/gen_site.py.</footer>' % html.escape(generated_from))
    parts.append("</div>")
    parts.append("<script>%s</script>" % SEARCH_JS)
    parts.append("</body></html>")
    return "\n".join(parts)


def build_search_json(db):
    return [
        {"name": e["name"], "brief": e["brief"], "docfile": e["docfile"],
         "anchor": anchor_id(e["docfile"], e["name"]), "status": e["status"]}
        for e in db.get("entries", [])
    ]


def main(argv=None):
    if not os.path.isfile(DB_PATH):
        print("error: %s not found -- run scripts/docs/extract.py first" % DB_PATH,
              file=sys.stderr)
        return 1
    with open(DB_PATH, "r", encoding="utf-8") as f:
        db = json.load(f)

    os.makedirs(SITE_DIR, exist_ok=True)
    with open(os.path.join(SITE_DIR, "index.html"), "w", encoding="utf-8") as f:
        f.write(build_page(db))
    with open(os.path.join(SITE_DIR, "search.json"), "w", encoding="utf-8") as f:
        json.dump(build_search_json(db), f, indent=2)

    print("site: rendered %d entries -> %s"
          % (len(db.get("entries", [])),
             os.path.relpath(os.path.join(SITE_DIR, "index.html"), REPO_ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
