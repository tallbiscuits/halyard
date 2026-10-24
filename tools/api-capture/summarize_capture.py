# SPDX-License-Identifier: GPL-3.0-or-later
"""Deduplicate capture/mainsail-api.jsonl into markdown tables, in first-seen order.

One row per JSON-RPC method, server notification and HTTP path, with the section it first
appeared in, how often it was called, and whether the backend answered with an error.
"""

import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(path):
    with open(path) as fh:
        return [json.loads(line) for line in fh if line.strip()]


def table(rows, title, key_name):
    out = [f"### {title}", "", f"| # | {key_name} | first seen | calls | sections | notes |",
           "|---|---|---|---|---|---|"]
    for i, (key, r) in enumerate(rows.items(), 1):
        errors = "; ".join(sorted(r["errors"])) or ""
        out.append(f"| {i} | `{key}` | {r['first']} | {r['count']} | "
                   f"{', '.join(r['labels'])} | {errors} |")
    return "\n".join(out) + "\n"


def tally(rows, key, entry):
    r = rows.setdefault(key, {"first": entry["label"], "count": 0, "labels": [], "errors": set()})
    r["count"] += 1
    if entry["label"] not in r["labels"]:
        r["labels"].append(entry["label"])
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default=str(HERE / "capture" / "mainsail-api.jsonl"))
    args = ap.parse_args()

    rpc, notify, http = {}, {}, {}
    unmatched = []  # error responses that answer no request id we sent (e.g. a rejected batch)
    for e in load(args.log):
        if e["kind"] == "rpc":
            r = tally(rpc, e["method"], e)
            if e.get("batch"):
                r["errors"].add("sent inside a JSON-RPC batch")
        elif e["kind"] == "response" and e.get("method") is None and e.get("error"):
            unmatched.append(f"- seq {e['seq']} [{e['label']}] id={e.get('id')}: {e['error']}")
        elif e["kind"] == "response" and e.get("method") in rpc:
            if e.get("error"):
                rpc[e["method"]]["errors"].add(e["error"])
            if e.get("shim"):
                rpc[e["method"]]["errors"].add("missing in backend; shimmed by proxy")
        elif e["kind"] == "notify":
            tally(notify, e["method"], e)
        elif e["kind"] == "http":
            # Collapse file names so each endpoint appears once.
            path = re.sub(r"^(\w+ /server/files/\w+/).+", r"\1…", e["path"])
            r = tally(http, path, e)
            if e.get("error"):
                r["errors"].add(e["error"])

    print(table(rpc, "JSON-RPC methods called by Mainsail", "method"))
    print(table(notify, "Notifications pushed by the backend", "notification"))
    print(table(http, "HTTP requests", "path"))
    if unmatched:
        print("### Error responses matching no request\n")
        print("\n".join(unmatched) + "\n")


if __name__ == "__main__":
    main()
