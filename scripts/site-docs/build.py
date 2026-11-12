#!/usr/bin/env python3
"""Build the static docs pages in site/docs/ from scripts/site-docs/pages/*.md.

Python standard library only. Page sources are Markdown with a few directives,
each alone on a line, so the site reuses the canonical prose and example files
in the repo instead of copying them by hand:

  {{section FILE "Heading" [level=N] [title="New heading"] [noheading]}}
      Pull one heading section (with its subsections) out of a Markdown file.
  {{between FILE "start text" "end text"}}
      Pull the lines from the one containing START to the one containing END.
  {{include FILE [lang]}}
      Embed a repo file verbatim as a labelled code block.

A fenced block with the info string `soundcheck FILE` runs its rows through
`build/monoblok --soundcheck` and renders the real transcript, so build the
binary first (cmake --build build --target monoblok).

Usage: scripts/site-docs/build.py
"""

import hashlib
import html
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PAGES_DIR = os.path.join(HERE, "pages")
OUT_DIR = os.path.join(ROOT, "site", "docs")
BIN = os.path.join(ROOT, "build", "monoblok")
GITHUB = "https://github.com/lexvicacom/monoblok"

# (slug, sidebar label, sidebar group)
PAGES = [
    ("index", "Introduction", "Start"),
    ("running", "Install and run", "Start"),
    ("patchbay", "Patchbay guide", "Patchbay"),
    ("forms", "Forms reference", "Patchbay"),
    ("agents", "Use Claude or Codex", "Patchbay"),
    ("examples", "Start here", "Examples"),
    ("examples-telemetry", "Sensors and telemetry", "Examples"),
    ("examples-market", "Market data", "Examples"),
    ("examples-nats", "NATS integration", "Examples"),
    ("examples-waf", "Filtering and privacy", "Examples"),
    ("examples-large", "Large patchbays", "Examples"),
    ("state", "LVC, stats and snapshots", "Server"),
    ("clients", "NATS clients, auth and HTTP", "Server"),
    ("bridge", "Export, import and JetStream", "Server"),
]

# Repo Markdown files that have a home on the docs site.
DOC_HOMES = {
    "docs/patchbay.md": "forms.html",
    "docs/patchbay-cheatsheet.md": "forms.html",
    "docs/overview.md": "index.html",
    "docs/README.md": "index.html",
    "docs/AGENTS_PATCHBAY.md": "agents.html",
    "README.md": "index.html",
}
ANCHOR_HOMES = {
    ("README.md", "run-it"): "running.html#install",
    ("docs/patchbay.md", "yaml-sugar-files"): "patchbay.html#yaml-sugar-files",
}


def die(message):
    sys.exit("build.py: " + message)


def read(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.isfile(path):
        die("missing file " + rel)
    with open(path, encoding="utf-8") as handle:
        return handle.read()


# ---------------------------------------------------------------- directives

def heading_of(line):
    match = re.match(r"^(#{1,6})\s+(.*?)\s*$", line)
    return (len(match.group(1)), match.group(2)) if match else None


def rewrite_links(text, src):
    """Point relative links in pulled prose at the docs site or at GitHub."""
    base = os.path.dirname(src)

    def fix(match):
        label, target = match.group(1), match.group(2)
        if re.match(r"^(https?:|mailto:|#)", target):
            return match.group(0)
        path, _, anchor = target.partition("#")
        rel = os.path.normpath(os.path.join(base, path)).replace(os.sep, "/")
        if (rel, anchor) in ANCHOR_HOMES:
            return "[%s](%s)" % (label, ANCHOR_HOMES[(rel, anchor)])
        if rel in DOC_HOMES:
            return "[%s](%s%s)" % (label, DOC_HOMES[rel], "#" + anchor if anchor else "")
        kind = "tree" if os.path.isdir(os.path.join(ROOT, rel)) else "blob"
        return "[%s](%s/%s/main/%s)" % (label, GITHUB, kind, rel)

    return re.sub(r"(?<!!)\[([^\]]+)\]\(([^)\s]+)\)", fix, text)


def pull_section(rel, wanted, level=None, title=None, noheading=False):
    lines = read(rel).split("\n")
    out, base, fenced = [], None, False
    for line in lines:
        if line.startswith("```"):
            fenced = not fenced
        head = None if fenced else heading_of(line)
        if base is None:
            if head and head[1] == wanted:
                base = head[0]
                shift = (level - base) if level else 0
                if not noheading:
                    out.append("#" * (base + shift) + " " + (title or head[1]))
            continue
        if head and head[0] <= base:
            break
        if head:
            line = "#" * min(6, head[0] + shift) + " " + head[1]
        out.append(line)
    if base is None:
        die('no heading "%s" in %s' % (wanted, rel))
    return rewrite_links("\n".join(out), rel)


def pull_between(rel, start, end):
    lines = read(rel).split("\n")
    first = next((i for i, line in enumerate(lines) if start in line), None)
    if first is None:
        die('no line containing "%s" in %s' % (start, rel))
    last = next((i for i in range(first, len(lines)) if end in lines[i]), None)
    if last is None:
        die('no line containing "%s" after "%s" in %s' % (end, start, rel))
    return rewrite_links("\n".join(lines[first:last + 1]), rel)


def include_file(rel, lang):
    lang = lang or {".edn": "edn", ".yml": "yaml", ".yaml": "yaml", ".json": "json", ".sh": "sh"}.get(os.path.splitext(rel)[1], "text")
    return "```%s file=%s\n%s\n```" % (lang, rel, read(rel).rstrip("\n"))


def expand_directives(source):
    out = []
    for line in source.split("\n"):
        match = re.match(r"^\{\{(\w+)\s+(.*)\}\}\s*$", line)
        if not match:
            out.append(line)
            continue
        name, rest = match.group(1), match.group(2)
        quoted = re.findall(r'(?<!=)"([^"]*)"', rest)
        options = dict(re.findall(r'(\w+)="([^"]*)"', rest))
        options.update(re.findall(r"(\w+)=(\d+)", rest))
        words = rest.split()
        if name == "section":
            level = int(options["level"]) if "level" in options else None
            out.append(pull_section(words[0], quoted[0], level, options.get("title"), "noheading" in words))
        elif name == "between":
            out.append(pull_between(words[0], quoted[0], quoted[1]))
        elif name == "include":
            out.append(include_file(words[0], words[1] if len(words) > 1 else None))
        else:
            die("unknown directive " + name)
    return "\n".join(out)


# -------------------------------------------------------------- highlighting

def esc(text):
    return html.escape(text, quote=False)


def span(cls, text):
    return '<span class="%s">%s</span>' % (cls, esc(text))


def highlight_with(pattern, classify, code):
    out, pos = [], 0
    for match in pattern.finditer(code):
        out.append(esc(code[pos:match.start()]))
        out.append(classify(match))
        pos = match.end()
    out.append(esc(code[pos:]))
    return "".join(out)


EDN = re.compile(r'(?P<com>;[^\n]*)|(?P<str>"(?:\\.|[^"\\])*")|(?P<call>\()(?P<op>[^\s()\[\]"]+)|(?P<kw>(?<![\w.:]):[A-Za-z][\w-]*)|(?P<num>(?<![\w.>-])-?\d+(?:\.\d+)?(?![\w.]))')
YAML = re.compile(r'(?P<com>(?:^|(?<=\s))#[^\n]*)|(?P<str>"(?:\\.|[^"\\])*")|(?P<key>^[ \t]*(?:- )?[A-Za-z][\w-]*:(?=\s|$))|(?P<bind>\$[a-z][\w?-]*)|(?P<num>(?<![\w.-])-?\d+(?:\.\d+)?(?![\w.]))', re.M)
JSON = re.compile(r'(?P<str>"(?:\\.|[^"\\])*")|(?P<num>(?<![\w.-])-?\d+(?:\.\d+)?(?![\w.]))')
SHELL = re.compile(r'(?P<com>(?:^|(?<=\s))#[^\n]*)|(?P<str>\'[^\'\n]*\'|"(?:\\.|[^"\\\n])*")', re.M)


def classify_edn(match):
    if match.group("call"):
        return "(" + span("tok-op", match.group("op"))
    kind = match.lastgroup
    return span("tok-" + kind, match.group(kind))


def classify_plain(match):
    kind = match.lastgroup
    return span("tok-" + kind, match.group(kind))


def highlight(code, lang):
    if lang in ("edn", "clojure"):
        return highlight_with(EDN, classify_edn, code)
    if lang in ("yaml", "yml"):
        return highlight_with(YAML, classify_plain, code)
    if lang == "json":
        return highlight_with(JSON, classify_plain, code)
    if lang in ("sh", "bash"):
        return highlight_with(SHELL, classify_plain, code)
    return esc(code)


def soundcheck(rel, rows):
    if not os.access(BIN, os.X_OK):
        die("build/monoblok is needed for soundcheck blocks: cmake --build build --target monoblok")
    done = subprocess.run(
        [BIN, "--soundcheck", "--soundcheck-label", "--soundcheck-linger-ms", "0", os.path.join(ROOT, rel)],
        input="\n".join(rows) + "\n", capture_output=True, text=True, timeout=30)
    if done.returncode != 0:
        die("soundcheck failed for %s: %s" % (rel, done.stderr.strip()))
    lines = []
    for line in done.stdout.rstrip("\n").split("\n"):
        kind, _, row = line.partition("|")
        subject, _, payload = row.partition("|")
        text = "%s  %s  %s" % ("in " if kind == "in" else "out", subject, payload)
        lines.append(span("sc-in" if kind == "in" else "sc-out", text))
    caption = 'soundcheck · <a href="%s/blob/main/%s" target="_blank" rel="noopener noreferrer">%s</a>' % (GITHUB, rel, esc(rel))
    return '<figure class="code"><figcaption><span>%s</span></figcaption><pre><code>%s</code></pre></figure>' % (caption, "\n".join(lines))


def code_block(info, body):
    words = info.split()
    lang = words[0] if words else ""
    if lang == "soundcheck":
        return soundcheck(words[1], [row for row in body.split("\n") if row.strip()])
    rel = next((word[5:] for word in words if word.startswith("file=")), None)
    label = {"edn": "EDN", "yaml": "YAML", "yml": "YAML", "json": "JSON", "sh": "shell", "bash": "shell", "nginx": "nginx"}.get(lang, lang or "text")
    if rel:
        label = '<a href="%s/blob/main/%s" target="_blank" rel="noopener noreferrer">%s</a>' % (GITHUB, rel, esc(rel))
    return '<figure class="code"><figcaption><span>%s</span></figcaption><pre><code>%s</code></pre></figure>' % (label, highlight(body, lang))


# ------------------------------------------------------------------ markdown

def slugify(text):
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).lower()
    return re.sub(r"[^a-z0-9]+", "-", text).strip("-") or "section"


def inline(text):
    codes = []

    def stash(match):
        codes.append("<code>%s</code>" % esc(match.group(2)))
        return "\x00%d\x00" % (len(codes) - 1)

    text = re.sub(r"(`+)(.+?)\1", stash, text)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
    text = esc(text)

    def link(match):
        label, target = match.group(1), html.unescape(match.group(2))
        external = re.match(r"^(https?:|mailto:)", target) is not None
        attrs = ' target="_blank" rel="noopener noreferrer"' if external and not target.startswith("mailto:") else ""
        return '<a href="%s"%s>%s</a>' % (html.escape(target), attrs, label)

    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    text = re.sub(r"\{tag:(\w+)\}", lambda m: '<span class="tag tag-%s">%s</span>' % (m.group(1).lower(), m.group(1)), text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![\w*])\*([^*\s][^*]*?)\*(?![\w*])", r"<em>\1</em>", text)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], text)


def table_cells(line):
    held = []

    def stash(match):
        held.append(match.group(0))
        return "\x01%d\x01" % (len(held) - 1)

    line = re.sub(r"(`+).+?\1", stash, line.strip())
    cells = [cell.strip() for cell in line.strip("|").split("|")]
    return [re.sub(r"\x01(\d+)\x01", lambda m: held[int(m.group(1))], cell) for cell in cells]


LIST_ITEM = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


class Page:
    """One rendered page: body HTML plus the headings found on the way."""

    def __init__(self):
        self.ids = set()
        self.title = None
        self.sections = []  # (id, text) for each h2

    def heading(self, level, text):
        rendered = inline(text)
        if level == 1:
            self.title = html.unescape(re.sub(r"<[^>]+>", "", rendered))
            return "<h1>%s</h1>" % rendered
        base = ident = slugify(rendered)
        count = 2
        while ident in self.ids:
            ident = "%s-%d" % (base, count)
            count += 1
        self.ids.add(ident)
        if level == 2:
            self.sections.append((ident, html.unescape(re.sub(r"<[^>]+>", "", rendered))))
        return '<h%d id="%s">%s<a class="anchor" href="#%s" aria-label="Link to this section">#</a></h%d>' % (level, ident, rendered, ident, level)

    def render(self, source):
        lines = source.split("\n")
        out, i = [], 0
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                i += 1
                continue
            if line.startswith("```"):
                info, body = line[3:].strip(), []
                i += 1
                while i < len(lines) and not lines[i].startswith("```"):
                    body.append(lines[i])
                    i += 1
                i += 1
                out.append(code_block(info, "\n".join(body)))
                continue
            head = heading_of(line)
            if head:
                out.append(self.heading(*head))
                i += 1
                continue
            if line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|\s*$", lines[i + 1]):
                header = table_cells(line)
                rows = []
                i += 2
                while i < len(lines) and lines[i].startswith("|"):
                    cells = table_cells(lines[i])
                    if rows and len(cells) > 2 and not cells[0] and not cells[1]:
                        # A continuation row: fold its cells into the row above.
                        for col, cell in enumerate(cells):
                            if cell and col < len(rows[-1]):
                                rows[-1][col] += "<br>" + inline(cell)
                    else:
                        rows.append([inline(cell) for cell in cells])
                    i += 1
                body = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % cell for cell in row) for row in rows)
                out.append('<div class="table-wrap"><table><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>' % (
                    "".join("<th>%s</th>" % inline(cell) for cell in header), body))
                continue
            if line.startswith(">"):
                quote = []
                while i < len(lines) and lines[i].startswith(">"):
                    quote.append(re.sub(r"^>\s?", "", lines[i]))
                    i += 1
                out.append("<blockquote>%s</blockquote>" % self.render("\n".join(quote)))
                continue
            if line.lstrip().startswith("<") and not line.lstrip().startswith("<http"):
                raw = []
                while i < len(lines) and lines[i].strip():
                    raw.append(lines[i])
                    i += 1
                out.append("\n".join(raw))
                continue
            item = LIST_ITEM.match(line)
            if item and not item.group(1):
                ordered = item.group(2)[0].isdigit()
                items = []
                while i < len(lines):
                    item = LIST_ITEM.match(lines[i])
                    if item and not item.group(1):
                        items.append([item.group(3)])
                        i += 1
                    elif lines[i].strip() and items and (lines[i].startswith(" ") or not heading_of(lines[i])) and not lines[i].startswith(("```", "|", ">")):
                        items[-1].append(lines[i].strip())
                        i += 1
                    elif not lines[i].strip() and i + 1 < len(lines) and LIST_ITEM.match(lines[i + 1]) and not LIST_ITEM.match(lines[i + 1]).group(1):
                        i += 1
                    else:
                        break
                tag = "ol" if ordered else "ul"
                out.append("<%s>%s</%s>" % (tag, "".join("<li>%s</li>" % inline(" ".join(parts)) for parts in items), tag))
                continue
            para = []
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(("```", "#", "|", ">")) and not (LIST_ITEM.match(lines[i]) and not para):
                para.append(lines[i].strip())
                i += 1
            if para and para[0].startswith("{lede}"):
                out.append('<p class="lede">%s</p>' % inline(" ".join(para)[6:].strip()))
            elif para:
                out.append("<p>%s</p>" % inline(" ".join(para)))
            else:
                i += 1
        return "\n".join(out)


# ------------------------------------------------------------------ template

def asset_version(name):
    with open(os.path.join(OUT_DIR, name), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()[:12]


def sidebar(current, pages):
    out, group = [], None
    for slug, label, section in PAGES:
        if section != group:
            if group is not None:
                out.append("</ul>")
            out.append("<h2>%s</h2><ul>" % esc(section))
            group = section
        here = ' aria-current="page"' if slug == current else ""
        out.append('<li><a href="%s.html"%s>%s</a>' % (slug, here, esc(label)))
        if slug == current and pages[slug].sections:
            out.append('<ul class="toc">%s</ul>' % "".join(
                '<li><a href="#%s">%s</a></li>' % (ident, esc(text)) for ident, text in pages[slug].sections))
        out.append("</li>")
    out.append("</ul>")
    return "".join(out)


def pager(index):
    out = []
    if index > 0:
        out.append('<a class="prev" href="%s.html"><small>Previous</small>%s</a>' % (PAGES[index - 1][0], esc(PAGES[index - 1][1])))
    if index + 1 < len(PAGES):
        out.append('<a class="next" href="%s.html"><small>Next</small>%s</a>' % (PAGES[index + 1][0], esc(PAGES[index + 1][1])))
    return '<nav class="pager" aria-label="Previous and next page">%s</nav>' % "".join(out)


TEMPLATE = """<!doctype html>
<html lang="en-GB">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} | monoblok docs</title>
<meta name="description" content="{description}">
<meta name="theme-color" content="#f5f4ed">
<link rel="stylesheet" href="docs.css?v={css}">
</head>
<body>
<a class="skip" href="#main">Skip to content</a>
<header class="docs-bar">
<a class="docs-brand" href="index.html" aria-label="monoblok docs home">monoblok <span>docs</span></a>
<nav aria-label="Site">
<a href="../index.html">Main site</a>
<a href="../demo.html">Live demo</a>
<a class="optional" href="https://alexjreid.dev/tags/monoblok/" target="_blank" rel="noopener noreferrer">Blog</a>
<a href="https://github.com/lexvicacom/monoblok" target="_blank" rel="noopener noreferrer">GitHub</a>
</nav>
</header>
<div class="docs-shell">
<details class="docs-side" open>
<summary>Docs menu · {label}</summary>
<nav class="side-inner" aria-label="Docs">{sidebar}</nav>
</details>
<main id="main" class="docs-main">
<article class="docs-article">
<p class="kicker">{group}</p>
{body}
{pager}
<p class="docs-foot">MIT licensed · &copy; 2026 <a href="mailto:alex@lexvica.com">Alex Reid</a><br><a href="https://lexvica.com" target="_blank" rel="noopener noreferrer">Lexvica</a> offers solutions built with monoblok.</p>
</article>
</main>
</div>
<script src="docs.js?v={js}"></script>
</body>
</html>
"""


def package_patchbay_skill():
    """Ship the whole portable skill so readers need no repository checkout."""
    source = os.path.join(ROOT, ".agents", "skills", "patchbay-authoring")
    output = os.path.join(ROOT, "site", "downloads", "patchbay-authoring.zip")
    if not os.path.isfile(os.path.join(source, "SKILL.md")):
        die("missing Patchbay skill entrypoint")
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for directory, dirs, files in os.walk(source):
            dirs.sort()
            for name in sorted(files):
                if name != "SKILL.md" and not name.endswith((".md", ".py")):
                    continue
                path = os.path.join(directory, name)
                relative = os.path.relpath(path, os.path.dirname(source)).replace(os.sep, "/")
                # Fixed metadata makes repeated docs builds byte-for-byte stable.
                entry = zipfile.ZipInfo(relative, date_time=(2026, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                with open(path, "rb") as handle:
                    archive.writestr(entry, handle.read())


def main():
    pages, bodies, descriptions = {}, {}, {}
    for slug, _, _ in PAGES:
        with open(os.path.join(PAGES_DIR, slug + ".md"), encoding="utf-8") as handle:
            source = handle.read()
        match = re.match(r"^description:\s*(.*)\n", source)
        if not match:
            die("%s.md must start with a description: line" % slug)
        descriptions[slug] = match.group(1).strip()
        page = Page()
        bodies[slug] = page.render(expand_directives(source[match.end():]))
        if not page.title:
            die("%s.md has no # title" % slug)
        pages[slug] = page

    problems = 0
    for slug, body in bodies.items():
        for target in re.findall(r'href="([^"]+)"', body):
            if re.match(r"^(https?:|mailto:|\.\./)", target):
                continue
            name, _, anchor = target.partition("#")
            other = name[:-5] if name else slug
            if other not in pages or (anchor and anchor not in pages[other].ids):
                print("warning: %s.md links to missing %s" % (slug, target))
                problems += 1

    package_patchbay_skill()
    css, js = asset_version("docs.css"), asset_version("docs.js")
    for index, (slug, label, group) in enumerate(PAGES):
        page = TEMPLATE.format(
            title=esc(pages[slug].title), description=html.escape(descriptions[slug]), css=css, js=js,
            label=esc(label), group=esc(group), sidebar=sidebar(slug, pages), body=bodies[slug], pager=pager(index))
        with open(os.path.join(OUT_DIR, slug + ".html"), "w", encoding="utf-8") as handle:
            handle.write(page)
    print("built %d pages into site/docs/ (%d link warnings)" % (len(PAGES), problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
