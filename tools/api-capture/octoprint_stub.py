# SPDX-License-Identifier: GPL-3.0-or-later
"""A throwaway OctoPrint impersonation, just enough for slicer upload.

Emulates exactly two endpoints:
  GET  /api/version      -> 200 {"api": "0.1", "server": "1.0"}   (no "text" field)
  POST /api/files/local  -> saves the upload to ./uploads/, 201

Every request -- including ones to paths we don't emulate -- is appended to
uploads/requests.jsonl, so we can see exactly what a slicer sends. Each upload
also gets <name>.fields.json next to the saved file.
"""

import argparse
import json
import os
import time
from pathlib import Path

from flask import Flask, jsonify, request

UPLOADS = Path(__file__).resolve().parent / "uploads"
REQUEST_LOG = UPLOADS / "requests.jsonl"

app = Flask(__name__)


def describe_request():
    """Everything the slicer sent, minus the file body."""
    entry = {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "remote": request.remote_addr,
        "method": request.method,
        "path": request.path,
        "query": request.args.to_dict(flat=False),
        "headers": dict(request.headers),
    }
    if request.mimetype == "multipart/form-data":
        entry["form"] = request.form.to_dict(flat=False)
        entry["files"] = {
            field: {
                "filename": f.filename,
                "content_type": f.content_type,
                "part_headers": dict(f.headers),
            }
            for field, f in request.files.items(multi=True)
        }
    elif request.content_length:
        entry["body"] = request.get_data(as_text=True)[:4096]
    return entry


def log(entry):
    UPLOADS.mkdir(exist_ok=True)
    with REQUEST_LOG.open("a") as fh:
        fh.write(json.dumps(entry) + "\n")
    print(json.dumps(entry, indent=2), flush=True)


@app.get("/api/version")
def version():
    log(describe_request())
    return jsonify({"api": "0.1", "server": "1.0"})


@app.post("/api/files/local")
def upload():
    entry = describe_request()
    f = request.files.get("file")
    if f is None or not f.filename:
        entry["result"] = "400: no file field"
        log(entry)
        return jsonify({"error": "No file included"}), 400

    # Flat store: keep only the basename; the requested folder is in the log.
    name = os.path.basename(f.filename.replace("\\", "/"))
    UPLOADS.mkdir(exist_ok=True)
    dest = UPLOADS / name
    f.save(dest)
    entry["saved_as"] = str(dest)
    entry["size_bytes"] = dest.stat().st_size

    (UPLOADS / f"{name}.fields.json").write_text(json.dumps(entry, indent=2) + "\n")
    log(entry)
    return jsonify({"files": {"local": {"name": name, "origin": "local"}}, "done": True}), 201


@app.route("/", defaults={"path": ""}, methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
@app.route("/<path:path>", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
def unknown(path):
    # Not emulated -- but record it, so we learn what else a slicer probes.
    entry = describe_request()
    entry["result"] = "404: not emulated"
    log(entry)
    return jsonify({"error": "not emulated by Halyard's OctoPrint stub"}), 404


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5000)
    args = ap.parse_args()
    print(f"OctoPrint stub on http://{args.host}:{args.port}  (uploads -> {UPLOADS})")
    app.run(host=args.host, port=args.port)
