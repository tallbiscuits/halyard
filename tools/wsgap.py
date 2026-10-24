#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Connects to Halyard's websocket the way Mainsail does, then uploads and downloads a large
file and reports the longest silence on the websocket during each. Mainsail closes a
websocket that has been silent for 10 s ("connection failed"), so anything near that fails.

    wsgap.py HOST [--size MB] [--file PATH]

Without --file it uploads harmless G-code (comments only) as wsgap-test.gcode and deletes it
afterwards. A given file keeps its name, is checked byte for byte after the download, and is
left in the library. HALYARD_API_KEY is sent as X-Api-Key when set."""
import argparse, asyncio, json, os, sys, time, urllib.parse, urllib.request, uuid
import websockets

LIMIT = 10.0  # Mainsail's heartbeat (webSocketClient.ts)


def headers():
    key = os.environ.get("HALYARD_API_KEY")
    return {"X-Api-Key": key} if key else {}


def upload(host, name, data):
    boundary = uuid.uuid4().hex
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + (
            f"\r\n--{boundary}\r\nContent-Disposition: form-data; name=\"print\"\r\n\r\nfalse\r\n"
            f"--{boundary}--\r\n").encode()
    req = urllib.request.Request(f"http://{host}/api/files/local", data=body, method="POST",
                                 headers={**headers(), "Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=300) as r:
        r.read()


def file_url(host, name):
    return f"http://{host}/server/files/gcodes/{urllib.parse.quote(name)}"


def download(host, name):
    with urllib.request.urlopen(urllib.request.Request(file_url(host, name), headers=headers()), timeout=300) as r:
        return r.read()


def delete(host, name):
    urllib.request.urlopen(urllib.request.Request(file_url(host, name), method="DELETE", headers=headers()),
                           timeout=60).read()


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("host")
    ap.add_argument("--size", type=float, default=7, help="MB of generated G-code (default 7)")
    ap.add_argument("--file", help="upload this file instead")
    a = ap.parse_args()
    if a.file:
        name, data = os.path.basename(a.file), open(a.file, "rb").read()
    else:
        line = b"; wsgap: a comment, nothing for the printer to do\n"
        name, data = "wsgap-test.gcode", b"M117 wsgap\n" + line * int(a.size * 1e6 / len(line))

    async with websockets.connect(f"ws://{a.host}/websocket", max_size=None,
                                  additional_headers=headers()) as ws:
        async def call(i, method, params=None):
            await ws.send(json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params or {}}))
            while True:
                m = json.loads(await ws.recv())
                if m.get("id") == i:
                    return m["result"]

        objs = (await call(1, "printer.objects.list"))["objects"]
        await call(2, "printer.objects.subscribe", {"objects": {o: None for o in objs}})
        stamps = []

        async def listen():
            async for _ in ws:
                stamps.append(time.monotonic())

        listener = asyncio.create_task(listen())

        def longest(t0, t1):  # silence inside [t0, t1], edges included
            ts = [t0] + [s for s in stamps if t0 < s < t1] + [t1]
            return max(b - a for a, b in zip(ts, ts[1:]))

        await asyncio.sleep(3)
        t = time.monotonic()
        idle = longest(t - 3, t)
        print(f"idle      : longest silence {idle:.1f} s")
        failed = idle >= LIMIT
        for what, job in (("upload", lambda: upload(a.host, name, data)),
                          ("download", lambda: download(a.host, name))):
            t0 = time.monotonic()
            got = await asyncio.to_thread(job)
            t1 = time.monotonic()
            await asyncio.sleep(1)  # a push queued at the very end still counts
            gap = longest(t0, t1 + 1)
            failed |= gap >= LIMIT
            print(f"{what:10}: {len(data) / 1e6:.1f} MB in {t1 - t0:.1f} s, longest silence {gap:.1f} s"
                  f"{'  <- Mainsail disconnects' if gap >= LIMIT else ''}")
        if got != data:
            print("download  : DIFFERS from what was uploaded")
            failed = True
        listener.cancel()
    if not a.file:
        delete(a.host, name)
    print("FAIL" if failed else "ok")
    return 1 if failed else 0


sys.exit(asyncio.run(main()))
