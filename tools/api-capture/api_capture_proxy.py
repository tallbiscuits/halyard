# SPDX-License-Identifier: GPL-3.0-or-later
"""Serve Mainsail and log every call it makes to a Moonraker-shaped backend.

    browser --> this proxy (:8080) --> Marlinraker (:7125)
                 |  serves Mainsail's static files
                 |  forwards /websocket, /server, /printer, /machine, /access, /api
                 '- appends every JSON-RPC message and HTTP path to capture/mainsail-api.jsonl

Mainsail connects to the host it was loaded from, so serving it here routes all of its
traffic through the log. Nothing here knows about Marlin; point --upstream at Halyard
later and the same capture works against it.

Section markers: GET /_mark?label=start-print  (tags subsequent entries with that label)
"""

import argparse
import asyncio
import json
import time
import traceback
from pathlib import Path

import aiohttp
from aiohttp import web

HERE = Path(__file__).resolve().parent
API_PREFIXES = ("/server/", "/printer/", "/machine/", "/access/", "/api/")
# Methods current Mainsail needs but Marlinraker (2023) lacks. Answered here so the UI can
# load; logged with shim=true so the capture still shows the backend doesn't provide them.
# server.webcams.list: without it Mainsail 2.19 hangs at "Initializing" (gui/webcams/initStore).
SHIMS = {
    "server.webcams.list": {"webcams": []},
}
HOP_HEADERS = {"host", "content-length", "transfer-encoding", "connection", "keep-alive",
               "upgrade", "content-encoding"}


class Capture:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.fh = path.open("a")
        self.label = "page-load"
        self.seq = 0
        self.pending = {}  # (conn, rpc id) -> method, to pair responses with requests

    def write(self, **entry):
        self.seq += 1
        entry = {"seq": self.seq, "t": round(time.time(), 3), "label": self.label, **entry}
        self.fh.write(json.dumps(entry) + "\n")
        self.fh.flush()
        if entry["kind"] != "notify" or entry.get("method") != "notify_proc_stat_update":
            brief = entry.get("method") or entry.get("path")
            err = f"  ERROR {entry['error']}" if entry.get("error") else ""
            print(f"[{entry['label']}] {entry['kind']:<8} {brief}{err}", flush=True)


def summarize_params(params):
    """Keep the log readable: record structure, not bulk data."""
    text = json.dumps(params)
    return params if len(text) <= 600 else text[:600] + "...(truncated)"


def make_app(upstream: str, www: Path, cap: Capture) -> web.Application:
    ws_upstream = upstream.replace("http", "ws", 1)
    session: aiohttp.ClientSession | None = None
    conn_counter = 0

    async def on_startup(_):
        nonlocal session
        session = aiohttp.ClientSession(auto_decompress=False)

    async def on_cleanup(_):
        await session.close()

    async def mark(request):
        cap.label = request.query.get("label", "unlabelled")
        cap.write(kind="mark", path=cap.label)
        return web.json_response({"label": cap.label})

    async def websocket(request):
        nonlocal conn_counter
        conn_counter += 1
        conn = conn_counter
        client = web.WebSocketResponse(max_msg_size=0)
        await client.prepare(request)
        cap.write(kind="ws-open", path=request.path_qs, conn=conn)

        async with session.ws_connect(ws_upstream + request.path_qs, max_msg_size=0) as up:
            async def browser_to_backend():
                async for msg in client:
                    if msg.type == aiohttp.WSMsgType.TEXT:
                        try:
                            m = json.loads(msg.data)
                        except ValueError:
                            cap.write(kind="ws-raw", conn=conn, path=msg.data[:200])
                            await up.send_str(msg.data)
                            continue
                        # JSON-RPC batches arrive as a list (Mainsail sends them after uploads).
                        batch = isinstance(m, list)
                        for req in (m if batch else [m]):
                            cap.write(kind="rpc", conn=conn, id=req.get("id"),
                                      method=req.get("method"), batch=batch or None,
                                      params=summarize_params(req.get("params")))
                        if not batch and m.get("method") in SHIMS:
                            result = SHIMS[m["method"]]
                            cap.write(kind="response", conn=conn, id=m.get("id"),
                                      method=m["method"], shim=True, error=None,
                                      result_keys=sorted(result))
                            await client.send_str(json.dumps(
                                {"jsonrpc": "2.0", "result": result, "id": m.get("id")}))
                            continue
                        for req in (m if batch else [m]):
                            cap.pending[(conn, req.get("id"))] = req.get("method")
                        await up.send_str(msg.data)
                    elif msg.type == aiohttp.WSMsgType.BINARY:
                        await up.send_bytes(msg.data)
                await up.close()

            async def backend_to_browser():
                async for msg in up:
                    if msg.type == aiohttp.WSMsgType.TEXT:
                        try:
                            parsed = json.loads(msg.data)
                        except ValueError:
                            parsed = []
                        for m in (parsed if isinstance(parsed, list) else [parsed]):
                            if "method" in m:
                                cap.write(kind="notify", conn=conn, method=m["method"])
                            else:
                                method = cap.pending.pop((conn, m.get("id")), None)
                                err = m.get("error")
                                cap.write(kind="response", conn=conn, id=m.get("id"),
                                          method=method, batch=isinstance(parsed, list) or None,
                                          error=err and f"{err.get('code')} {err.get('message')}",
                                          result_keys=sorted(m["result"].keys())
                                          if isinstance(m.get("result"), dict) else None)
                        await client.send_str(msg.data)
                    elif msg.type == aiohttp.WSMsgType.BINARY:
                        await client.send_bytes(msg.data)
                await client.close()

            async def logged(direction, coro):
                # Report a dead relay direction immediately; otherwise the other direction
                # keeps the page looking alive while requests silently stop.
                try:
                    await coro
                except Exception as exc:
                    traceback.print_exc()
                    cap.write(kind="proxy-error", conn=conn, path=direction, error=repr(exc))
                    await client.close()
                    await up.close()

            await asyncio.gather(logged("browser->backend", browser_to_backend()),
                                 logged("backend->browser", backend_to_browser()))
        cap.write(kind="ws-close", path=request.path_qs, conn=conn)
        return client

    async def http_proxy(request):
        body = await request.read()
        headers = {k: v for k, v in request.headers.items() if k.lower() not in HOP_HEADERS}
        form_fields = None
        if request.content_type == "multipart/form-data":
            form_fields = "multipart (%d bytes)" % len(body)
        elif body:
            form_fields = body[:600].decode(errors="replace")
        try:
            up = await session.request(request.method, upstream + request.path_qs,
                                       headers=headers, data=body or None)
            data = await up.read()
            up.release()
        except aiohttp.ClientError as exc:
            cap.write(kind="http", method=None, path=f"{request.method} {request.path}",
                      status=502, error=f"upstream unreachable: {exc}")
            return web.Response(status=502, text="upstream unreachable")
        cap.write(kind="http", method=None, path=f"{request.method} {request.path}",
                  query=request.query_string or None, body=form_fields,
                  status=up.status,
                  error=f"HTTP {up.status}" if up.status >= 400 else None)
        out_headers = {k: v for k, v in up.headers.items()
                       if k.lower() not in HOP_HEADERS - {"content-encoding"}}
        return web.Response(status=up.status, body=data, headers=out_headers)

    async def static(request):
        rel = request.match_info["tail"] or "index.html"
        target = (www / rel).resolve()
        if www in target.parents and target.is_file():
            return web.FileResponse(target)
        return web.FileResponse(www / "index.html")  # SPA fallback

    async def dispatch(request):
        if request.path == "/websocket":
            return await websocket(request)
        if request.path == "/_mark":
            return await mark(request)
        if request.path.startswith(API_PREFIXES):
            return await http_proxy(request)
        return await static(request)

    app = web.Application(client_max_size=1024 ** 3)
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    app.router.add_route("*", "/{tail:.*}", dispatch)
    return app


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--upstream", default="http://127.0.0.1:7125")
    ap.add_argument("--www", default=str(HERE / "mainsail"))
    ap.add_argument("--log", default=str(HERE / "capture" / "mainsail-api.jsonl"))
    args = ap.parse_args()
    cap = Capture(Path(args.log))
    print(f"Mainsail on http://localhost:{args.port}  ->  {args.upstream}  (log: {args.log})")
    web.run_app(make_app(args.upstream, Path(args.www).resolve(), cap),
                port=args.port, print=None)
