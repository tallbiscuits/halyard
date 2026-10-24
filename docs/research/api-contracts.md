# API contracts Halyard must satisfy

Everything here was read from source or official docs, not inferred. Sources are named so
a later session can re-verify.

---

## 1. Slicer upload — OctoPrint shape

Verified in `OrcaSlicer/src/slic3r/Utils/OctoPrint.cpp` (`main` branch) and the
[OctoPrint file API docs](https://docs.octoprint.org/en/master/api/files.html).
PrusaSlicer, OrcaSlicer and Cura all speak this.

### Connection test

```
GET /api/version
X-Api-Key: <key>
```

Response JSON **must** contain an `api` key. The `text` key is optional — but if present,
`OctoPrint::validate_version_text()` requires it to start with `"OctoPrint"`:

```cpp
return version_text ? boost::starts_with(*version_text, "OctoPrint") : true;
```

**Simplest correct answer — omit `text` entirely:**

```json
{"api": "0.1", "server": "1.0"}
```

### Upload

```
POST /api/files/local
X-Api-Key: <key>
Content-Type: multipart/form-data
```

| Field | Notes |
|---|---|
| `file` | the G-code, with filename; part `Content-Type: application/octet-stream` |
| `print` | `"true"` / `"false"` — start immediately |
| `path` | parent folder, may be empty |
| `select` | optional in the OctoPrint API; **OrcaSlicer never sends it** |
| `plateindex` | OrcaSlicer only, and only when uploading a `.gcode.3mf` (1-based plate number); absent for plain `.gcode` |

Respond **201**:

```json
{"files": {"local": {"name": "x.gcode", "origin": "local"}}, "done": true}
```

### Observed from a real slicer (OrcaSlicer 2.4.2, 2026-09-22)

Captured by `tools/api-capture/octoprint_stub.py`; host type `Octo/Klipper`, plain `.gcode`.

- **Test button:** one `GET /api/version`. Nothing else is probed.
- **Every upload is preceded by its own `GET /api/version`**, then a single
  `POST /api/files/local`. No other paths are requested; no follow-up call to start a print.
- **Upload:** form fields `print=false`, `path=""`, then `file`. **Upload and Print:** identical
  except `print=true` — no `select`, no separate start request. The server must start the
  print itself when `print=true`.
- **No `plateindex`** for plain `.gcode` — in Orca's source it is only added when non-empty,
  which happens for `.gcode.3mf` uploads.
- **`X-Api-Key` is sent on every request, `/api/version` included, when a key is set**
  (verified with key `halyard`). With the API-key field blank the header is omitted entirely.
  Halyard accepts requests without a key from trusted ranges (D11).
  User-Agent is `OrcaSlicer/2.4.2`.
- Re-sending the same file uses the same filename, so an upload replaces the file of that name.
- G-code header carries `; estimated printing time (normal mode) = …`. **Thumbnails are
  absent** unless enabled in the Orca printer profile (`thumbnails = ` was empty here) —
  metadata extraction must tolerate that.

### Also worth implementing

- `POST /api/login` → `{}` — Cura's OctoPrint plugin expects it (one line).
- `GET /api/job`, `/api/printer`, `/api/connection` — only needed for Cura's status panel.

---

## 2. Web UI — Moonraker shape

Verified against [Moonraker's docs](https://moonraker.readthedocs.io/en/latest/) and
`OrcaSlicer/src/slic3r/Utils/Moonraker.cpp`.

### Websocket

`ws://host/websocket`, JSON-RPC 2.0.

```json
→ {"jsonrpc":"2.0","method":"printer.objects.subscribe","params":{...},"id":354}
← {"jsonrpc":"2.0","result":{...},"id":354}
← {"jsonrpc":"2.0","method":"notify_status_update","params":[{...}, <eventtime>]}
```

Key pushes: `notify_klippy_ready`, `notify_klippy_disconnected`, `notify_status_update`.

> **Measured — see [`mainsail-api-log.md`](mainsail-api-log.md), which supersedes
> the lists below for scoping.** Two things this section missed: Mainsail 2.19 sends **JSON-RPC
> batches** (a JSON array of requests) and hangs at startup unless **`server.webcams.list`**
> succeeds. Several database and upload calls also go over **HTTP**, not the websocket.

### Methods to implement (from Marlinraker's `src/api/executors/`, 52 total)

Handshake — `server.connection.identify`, `server.info`, `printer.info`, `server.config`
Objects — `printer.objects.list`, `printer.objects.query`, `printer.objects.subscribe`
Files — `server.files.list`, `server.files.metadata`, `server.files.get_directory`,
`server.files.delete_file`, `server.files.move`, `server.files.copy`
Print — `printer.print.start`, `.pause`, `.resume`, `.cancel`
G-code — `printer.gcode.script`, `printer.gcode.help`, `server.gcode_store`
Safety — `printer.emergency_stop`, `printer.firmware_restart`, `printer.restart`
UI state — `server.database.get_item` / `post_item` / `delete_item` / `list`
Cosmetic — `machine.system_info`, `machine.proc_stats`, `server.temperature_store`,
`server.history.*`, `server.announcements.list`, `machine.update.status`

### Printer objects to fake (from `src/printer/objects/`, 16 total)

`webhooks` · `print_stats` · `virtual_sdcard` · `toolhead` · `extruder` (heaters) ·
`heater_bed` · `temperature` · `gcode_move` · `display_status` · `idle_timeout` ·
`pause_resume` · `fan` · `configfile` · `bed_mesh` · `motion_report` · `system_stats`

Start with the first ten; the rest can be constant stubs.

### Slicer upload over Moonraker (if ever wanted)

`GET /server/info` (shape-checked by Orca), optional `GET /server/files/roots`,
`POST /server/files/upload` (fields `file`, `root`, `print`), then a **separate**
`POST /printer/print/start` with `{"filename": ...}`. Larger surface than OctoPrint's for
the same result — hence D4.

---

## 3. Reference implementations worth reading

| Project | Use |
|---|---|
| [Marlinraker](https://github.com/phm07/marlinraker) | Moonraker emulation for Marlin. The specification. Node.js. |
| [WirelessPrinting](https://github.com/kelv1n9/WirelessPrinting) | ESP32 OctoPrint emulation, streams with `N…*checksum`. No auth; ESP32-only; Yandex cruft. |
| [ESP3D](https://github.com/luc-github/ESP3D) | Has SD upload + `GcodeHost::processFile` streaming. **No OctoPrint compatibility** — grep of branch `3.0` for `octoprint` / `api/version` returns nothing; requests [#216](https://github.com/luc-github/ESP3D/issues/216), [#522](https://github.com/luc-github/ESP3D/issues/522) closed unimplemented. |
| `buildroot/share/scripts/MarlinBinaryProtocol.py` | In the Marlin tree. Complete `M28 B1` client, 446 lines. Port this. |

## 4. Web UI release sizes (for storage planning)

| UI | Release zip |
|---|---|
| Mainsail v2.19.0 | 3.1 MB |
| Fluidd v1.37.5 | 4.4 MB |
