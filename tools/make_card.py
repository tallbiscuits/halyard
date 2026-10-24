#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare Halyard's microSD card on a PC: halyard.cfg plus Mainsail, gzipped as Halyard
serves it. Standard library only, so it runs the same on Windows, macOS and Linux.

    make_card.py MAINSAIL_DIR OUT [--ssid NAME --password PASS]

MAINSAIL_DIR is Mainsail's release, unzipped (it holds index.html). OUT is the card's root
(a mounted, FAT32-formatted card, or any folder) or a .zip file for a release download, to
be unzipped onto the card. halyard.cfg is written only if OUT has none, so re-running keeps
settings made since.
"""
import argparse
import gzip
import io
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "..", "card", "halyard.cfg")
WWW = ".halyard/www"  # where Halyard serves Mainsail from (firmware/src/main.cpp)


def gzipped(data):
    """Mainsail's files, each gzipped where that saves at least a tenth (as ./run.sh webui)."""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as g:
        g.write(data)
    z = buf.getvalue()
    return (z, True) if len(z) * 10 < len(data) * 9 else (data, False)


def config_text(ssid, password):
    with open(TEMPLATE, encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = []
    section = ""
    for line in lines:
        t = line.strip()
        if t.startswith("["):
            section = t
        if section == "[wifi]" and t == "ssid:" and ssid:
            line = "ssid: " + ssid
        if section == "[wifi]" and t == "password:" and password:
            line = "password: " + password
        out.append(line)
    return "\n".join(out)


def card_files(mainsail, ssid, password):
    """(path on the card, bytes) for everything Halyard needs."""
    if not os.path.isfile(os.path.join(mainsail, "index.html")):
        sys.exit(f"{mainsail}: no index.html; unzip Mainsail's release there first")
    yield "halyard.cfg", config_text(ssid, password).encode("utf-8")
    yield "gcodes/", b""
    for root, _, names in os.walk(mainsail):
        for n in sorted(names):
            src = os.path.join(root, n)
            rel = os.path.relpath(src, mainsail).replace(os.sep, "/")
            with open(src, "rb") as f:
                data, packed = gzipped(f.read())
            yield f"{WWW}/{rel}{'.gz' if packed else ''}", data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mainsail", help="Mainsail's release, unzipped")
    ap.add_argument("out", help="the card's root folder, or a .zip to create")
    ap.add_argument("--ssid", default="", help="the WiFi network Halyard joins")
    ap.add_argument("--password", default="", help="its password")
    a = ap.parse_args()
    if a.password and not a.ssid:
        sys.exit("--password without --ssid")
    if a.password and not 8 <= len(a.password) <= 64:
        sys.exit("a WiFi password has 8 to 64 characters")

    files = list(card_files(a.mainsail, a.ssid, a.password))
    if a.out.lower().endswith(".zip"):
        with zipfile.ZipFile(a.out, "w", zipfile.ZIP_STORED) as z:  # mostly gzipped already
            for path, data in files:
                z.writestr(path, data)
        print(f"{a.out}: {len(files) - 2} Mainsail files and halyard.cfg; unzip it onto the card")
        return
    old_ui = os.path.join(a.out, *WWW.split("/"))
    if os.path.isdir(old_ui):  # a new Mainsail replaces the old one whole
        for root, dirs, names in os.walk(old_ui, topdown=False):
            for n in names:
                os.remove(os.path.join(root, n))
            for d in dirs:
                os.rmdir(os.path.join(root, d))
    kept = False
    for path, data in files:
        dst = os.path.join(a.out, *path.split("/"))
        if path.endswith("/"):
            os.makedirs(dst, exist_ok=True)
            continue
        if path == "halyard.cfg" and os.path.exists(dst):
            kept = True
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    print(f"{a.out}: Mainsail ({len(files) - 2} files) written; halyard.cfg "
          + ("kept as it was" if kept else "written" + ("" if a.ssid else ": fill in [wifi] ssid and password")))


if __name__ == "__main__":
    main()
