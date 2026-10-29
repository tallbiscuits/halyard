#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a built site folder (./run.sh site writes dist/site/) before it is deployed.

Every internal link, image, script, stylesheet and CSS url() must resolve to a file, every
#anchor to an id in its page, and the flasher's manifest.json to its image. Links into the
repository may name only the published documents (README, LICENSE, docs/firmware.md and
docs/decisions.md): PLAN.md, CLAUDE.md and the rest of docs/ are not published.
Placeholders (@VERSION@, @RELEASES@) must all be filled in.
    check_site.py DIR
"""
import json
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

REPO = "https://github.com/tallbiscuits/halyard/"
PUBLISHED_DOCS = {"docs/firmware.md", "docs/decisions.md"}
URL_ATTRS = {"href", "src", "manifest"}


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids, self.urls = set(), []

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name in ("id", "name") and value:
                self.ids.add(value)
            if name in URL_ATTRS and value is not None:
                self.urls.append((tag, name, value))


def main(root):
    root = Path(root).resolve()
    errors, pages = [], {}
    for f in sorted(root.rglob("*.html")):
        p = Page()
        p.feed(f.read_text(encoding="utf-8"))
        pages[f] = p

    def page_ids(f):
        if f not in pages:
            p = Page()
            p.feed(f.read_text(encoding="utf-8"))
            pages[f] = p
        return pages[f].ids

    def check(src, url, what):
        parts = urlsplit(url)
        if parts.scheme in ("http", "https"):
            if url.startswith(REPO):
                m = re.match(r"(?:blob|tree)/[^/]+/(.*)", url[len(REPO):])
                path = m.group(1) if m else ""
                if path.startswith("docs/") and path not in PUBLISHED_DOCS or \
                        path.split("/")[0] in ("PLAN.md", "CLAUDE.md"):
                    errors.append(f"{src.relative_to(root)}: links to unpublished {path}")
            return
        if parts.scheme in ("mailto", "data") or url.startswith("//"):
            return
        if parts.scheme:
            errors.append(f"{src}: unexpected {what} {url}")
            return
        target = src if not parts.path else (src.parent / unquote(parts.path)).resolve()
        if parts.path.endswith("/") or target.is_dir():
            target = target / "index.html"
        if root not in target.parents and target != root:
            errors.append(f"{src}: {what} {url} leaves the site")
            return
        if not target.is_file():
            errors.append(f"{src.relative_to(root)}: {what} {url}: no {target.relative_to(root)}")
            return
        if parts.fragment and target.suffix == ".html" and unquote(parts.fragment) not in page_ids(target):
            errors.append(f"{src.relative_to(root)}: {url}: no id {parts.fragment!r} in {target.relative_to(root)}")

    for f, p in list(pages.items()):
        text = f.read_text(encoding="utf-8")
        for ph in re.findall(r"@[A-Z]+@", text):
            errors.append(f"{f.relative_to(root)}: placeholder {ph} not filled in")
        for tag, name, url in p.urls:
            check(f, url, f"<{tag} {name}>")
    for css in sorted(root.rglob("*.css")):
        for url in re.findall(r"url\(\s*['\"]?([^'\")]+)", css.read_text(encoding="utf-8")):
            check(css, url, "url()")
    for man in sorted(root.rglob("manifest.json")):
        for build in json.loads(man.read_text())["builds"]:
            for part in build["parts"]:
                if not (man.parent / part["path"]).is_file():
                    errors.append(f"{man.relative_to(root)}: no {part['path']}")

    for e in errors:
        print("check_site:", e, file=sys.stderr)
    links = sum(len(p.urls) for p in pages.values())
    print(f"check_site: {len(pages)} pages, {links} links: "
          + (f"{len(errors)} problems" if errors else "all resolve"))
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
