#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Halyard's config reference: halyard.cfg's own comments, as a page Mainsail links to.

    config_reference.py HALYARD_CFG OUT_HTML

The card's halyard.cfg (card/halyard.cfg, which a test keeps equal to what the firmware
writes) is the one source. Comments before the first section introduce the page. In a
section, the comments above a setting describe it, and its value in the file is its default.
Each section gets an anchor (#wifi), which Mainsail's editor links to from [wifi].
Standard library only.
"""
import html
import sys


def parse(text):
    intro, sections, notes = [], [], []
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("#"):
            notes.append(line.lstrip("#").strip())
        elif line.startswith("[") and line.endswith("]"):
            if not sections:
                intro, notes = intro + notes, []
            sections.append({"name": line[1:-1].strip(), "notes": notes, "settings": []})
            notes = []
        elif ":" in line and sections:
            key, value = line.split(":", 1)
            sections[-1]["settings"].append((key.strip(), value.strip(), notes))
            notes = []
        elif not line and not sections and notes:
            intro, notes = intro + notes, []
    return intro, sections


def page(intro, sections):
    e = html.escape
    out = []
    for s in sections:
        rows = []
        for key, value, notes in s["settings"]:
            default = f"<code>{e(value)}</code>" if value else "<span class=muted>empty</span>"
            rows.append(f"<dt id='{e(s['name'])}.{e(key)}'><code>{e(key)}</code></dt>"
                        f"<dd>{f'<p>{e(chr(32).join(notes))}</p>' if notes else ''}"
                        f"<p class=muted>Default: {default}</p></dd>")
        head = f"<p>{e(' '.join(s['notes']))}</p>" if s["notes"] else ""
        out.append(f"<section id='{e(s['name'])}'><h2>[{e(s['name'])}]</h2>{head}<dl>{''.join(rows)}</dl></section>")
    toc = " · ".join(f"<a href='#{e(s['name'])}'>[{e(s['name'])}]</a>" for s in sections)
    return f"""<!doctype html>
<html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width, initial-scale=1">
<title>halyard.cfg Reference</title>
<style>
:root {{ --bg:#f6f7f9; --card:#fff; --text:#1d232b; --muted:#5b6573; --line:#dde2e8; --accent:#1f6feb; }}
@media (prefers-color-scheme: dark) {{
  :root {{ --bg:#121212; --card:#1e1e1e; --text:#e6e9ed; --muted:#9aa4b1; --line:#2f3338; --accent:#4c8dff; }}
}}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--bg); color:var(--text); font:16px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }}
main {{ max-width:46rem; margin:0 auto; padding:2rem 16px 3rem; }}
h1 {{ margin:0 0 .5rem; font-size:1.7rem; }}
nav {{ margin:1rem 0 1.5rem; }}
a {{ color:var(--accent); }}
section {{ background:var(--card); border:1px solid var(--line); border-radius:10px; padding:1rem 1.3rem; margin-bottom:1rem; scroll-margin-top:1rem; }}
section:target {{ border-color:var(--accent); }}
h2 {{ margin:0 0 .4rem; font-size:1.15rem; font-family:ui-monospace,monospace; }}
dl {{ margin:0; }} dt {{ margin-top:.9rem; font-weight:600; }} dd {{ margin:.2rem 0 0; }} dd p {{ margin:.15rem 0; }}
code {{ font-family:ui-monospace,monospace; font-size:.92em; overflow-wrap:anywhere; }}
.muted {{ color:var(--muted); font-size:.93rem; }}
</style></head><body><main>
<h1>halyard.cfg</h1>
<p>{e(' '.join(intro))}</p>
<p class=muted>Settings Halyard doesn't know are reported as warnings and otherwise ignored. After a change,
SAVE &amp; RESTART reads the file again.</p>
<nav>{toc}</nav>
{''.join(out)}
</main></body></html>
"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8-sig") as f:
        intro, sections = parse(f.read())
    if not sections:
        sys.exit(f"{sys.argv[1]}: no sections")
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(page(intro, sections))


if __name__ == "__main__":
    main()
