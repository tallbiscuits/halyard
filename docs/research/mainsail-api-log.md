# Mainsail API log — what Mainsail actually calls

The scope of Halyard's Moonraker API, measured rather than guessed. Every JSON-RPC method, server
notification and HTTP path Mainsail used during one scripted session against a real printer.

| | |
|---|---|
| Captured | 2026-09-22 |
| UI | Mainsail **v2.19.0** (release zip, unmodified) |
| Backend | Marlinraker v0.2.3-alpha (Docker, its own Dockerfile) |
| Printer | Ender 3, BTT SKR 2, **Marlin 2.1.2.8**, USB `/dev/ttyACM0` (reports 36 `M115` capabilities) |
| Method | `tools/api-capture/api_capture_proxy.py` serves Mainsail and relays everything to Marlinraker, logging each message |

**Reproduce:** `tools/api-capture/run.sh marlinraker /dev/ttyACM0`, then `tools/api-capture/run.sh proxy`, open
`http://localhost:8080`, tag sections with `GET /_mark?label=…`, then `tools/api-capture/run.sh summarize`.
The capture was taken from a clean state: fresh browser profile, empty Marlinraker database and
G-code folder.

**Session, in order:** (1) first page load → (2) 30 s idle on dashboard → (3) open G-Code
Files → (4) upload a file from the file list → (5) send `M115` from the console → (6) turn on
the "Display CANCEL_PRINT" UI setting → (7) start the file from the list → (8) 20 s printing →
(9) cancel → (10) 15 s idle. The test file had no heater or motion commands; Marlinraker's
cancel macro homed X/Y.

12. **Macros, and the manual-probe dialog** (read from Mainsail 2.19's bundle, 2026-09-27).
    The macro panel lists every printer object named `gcode_macro <NAME>` (so it must be in
    `printer.objects.list`), skipping names starting with `_` and sections with
    `rename_existing`. The tooltip is `gcode.commands[NAME].help`. The parameter fields are
    parsed out of `configfile.settings["gcode_macro <name>"].gcode` with the regex for
    `{params.X|default(v)|int}`. The manual-probe dialog opens while `manual_probe.is_active`
    is true and shows `z_position`, `z_position_lower` and `z_position_upper` (null shows
    `??????`); its buttons send `TESTZ Z=<n>`, `TESTZ Z=+|-|++|--`, `ACCEPT` and `ABORT`.
    The top bar shows SAVE CONFIG while `configfile.save_config_pending` is true, and it
    sends `SAVE_CONFIG`.

---

## Findings that shape the API

1. **`server.webcams.list` is mandatory.** Mainsail 2.19 calls it on every load, even though
   `webcams` is not in `server.info` components, and **hangs at "Initializing"** if it errors
   (`Response Error: Method not found (gui/webcams/initStore)`). Marlinraker lacks it; the
   capture proxy shims `{"webcams": []}`. Halyard: return an empty list.
2. **JSON-RPC batches are required.** After an upload Mainsail sends `server.files.metadata` as a
   JSON array (`emitBatch()` in `src/plugins/webSocketClient.ts`). Marlinraker cannot parse
   batches and answers with a single `400 Bad Request`, `id: 0`, so the new file's metadata
   silently never arrives. Halyard: accept an array, answer with an array.
3. **Mainsail subscribes to exactly what `printer.objects.list` returns.** Marlinraker lists 15
   objects, and Mainsail subscribed to all 15: `webhooks toolhead fan gcode_move motion_report
   system_stats heaters configfile print_stats virtual_sdcard idle_timeout pause_resume bed_mesh
   extruder heater_bed`. `display_status` (in the planned list) was never requested; `heaters`
   and `configfile` were. Halyard controls the surface through this list.
4. **Some database and file traffic is HTTP, not websocket.** `POST /server/database/item` at
   first load, `GET /server/database/list` + `GET /server/database/item` when the settings dialog
   opens, and `POST /server/files/upload` for uploads. The HTTP routes are part of the contract.
5. **Mainsail writes to the database from the very first load:** a `maintenance` init entry,
   `initVersion`, G-code viewer kinematics cache, console history, UI settings. Without
   persistence Mainsail forgets all of it on reboot, so `server.database.*` is needed from
   the start, and Halyard keeps it on its card.
6. **Only two objects are one-off queried:** `gcode.commands` (console autocomplete, instead of
   `printer.gcode.help`, which was never called) and `extruder.can_extrude`.
7. **The config root is probed and 404s are tolerated:** `server.files.get_directory
   {path: "config"}` plus `GET /server/files/config/.theme/{default,maintenance}.json` → 404.
   An empty `config` root is enough.
8. **"Upload & Print" (header button)** is the same `POST /server/files/upload`, and the upload
   itself starts the print. Seen in a preliminary session, not in the clean capture: the header
   button was clicked by mistake and the file printed to completion, even though the proxy's
   websocket relay had already failed, so no `printer.print.start` could have got through.
   Halyard's upload handler must honour a `print=true` form field (Moonraker's documented field;
   the proxy logged the multipart body's size, not its fields).
9. **Console output:** `printer.gcode.script {"script": "M115"}` succeeded, but Marlinraker did
   not forward M115's reply as `notify_gcode_response`. Only Marlinraker's own messages (macro
   `M118` echoes, its "Unknown command" warnings) reached the console. Halyard should forward printer replies.
10. **Version check is a warning only.** With Marlinraker, Mainsail shows a bell notification:
    "Dependency: Moonraker … Update Moonraker to at least v0.8.0-306." Source
    (`src/store/variables.ts`, `src/store/getters.ts`): it compares `server.info`
    `moonraker_version` against `minMoonrakerVersion = 'v0.8.0-306'`, splitting on `-` into a
    semver release and a build number. Marlinraker reports `v0.2.3-alpha`, hence the warning.
    Nothing else in Mainsail keys off this version; no feature is gated by it. The check is
    skipped entirely if the release part is not valid semver. Halyard: report a version at or
    above the minimum (it implements that API level), or a non-semver string. Either way,
    re-check the constant whenever the pinned Mainsail version changes. (Klipper's version gets
    the same treatment from `printer.info`'s `software_version`. Halyard's Mainsail skips
    that check when `printer.info`'s `app` isn't Klipper (patch 0014, 2026-10-10), so Halyard
    reports its own version there as it is.)

11. **The upload reply is not wrapped in `result`** (found against Halyard, 2026-09-27). Mainsail
    2.19's `files/uploadFile` reads `response.data.item.path`. With Moonraker's usual
    `{"result": {...}}` wrapper that throws, and Mainsail reports "Cannot upload file!" even
    though the file was stored. Answer `POST /server/files/upload` with the bare
    `{"item": {...}, "action": "create_file", ...}`, as Marlinraker does. Every other HTTP route
    is wrapped.

---

## JSON-RPC methods (websocket), in first-seen order

| # | Method | First seen | Calls | What Mainsail uses it for |
|---|---|---|---|---|
| 1 | `server.connection.identify` | page load | 1 | Registers the client (`mainsail`, `2.19.0`, `web`); returns `connection_id`. |
| 2 | `server.info` | page load | 2 | Klippy state, components, API version; called twice during init. |
| 3 | `server.config` | page load | 1 | Moonraker config; Mainsail reads which components are configured. |
| 4 | `machine.system_info` | page load | 1 | Host CPU/OS/network info for the Machine page. |
| 5 | `machine.proc_stats` | page load | 1 | Host load, memory, temperature; followed by `notify_proc_stat_update` pushes. |
| 6 | `server.database.list` | page load | 1 | Lists database namespaces (`{"root":"config"}`). |
| 7 | `server.history.list` | page load | 1 | Last 50 jobs for the History panel. |
| 8 | `server.history.totals` | page load | 3 | Job totals; re-fetched after print start and cancel. |
| 9 | `machine.update.status` | page load | 1 | Update-manager panel. A constant stub will do. |
| 10 | `server.files.get_directory` | page load | 2 | Lists the `gcodes` and `config` roots. |
| 11 | `server.webcams.list` | page load | 1 | Webcam config. **Must succeed or the UI hangs** (finding 1). |
| 12 | `printer.objects.query` | page load | 2 | One-off reads: `gcode.commands` (autocomplete), `extruder.can_extrude`. |
| 13 | `printer.info` | page load | 1 | Klipper state (`ready`/`error`) and hostname. |
| 14 | `server.gcode_store` | page load | 1 | Console backlog. |
| 15 | `printer.objects.list` | page load | 1 | Which printer objects exist; drives the subscription (finding 3). |
| 16 | `server.database.post_item` | page load | 6 | Persists UI state: viewer cache, maintenance init, console history, settings. |
| 17 | `printer.objects.subscribe` | page load | 1 | Subscribes to all 15 listed objects; powers `notify_status_update`. |
| 18 | `server.temperature_store` | page load | 1 | Temperature history for the chart. |
| 19 | `server.database.get_item` | page load | 1 | Loads the `mainsail` namespace (all UI settings). |
| 20 | `server.files.metadata` | upload | 2 | Slicer metadata for a file; **batched** after upload (finding 2), plain on print start. |
| 21 | `printer.gcode.script` | console | 1 | Runs a console command. |
| 22 | `printer.print.start` | start print | 1 | Starts a print: `{"filename": "…"}`. |
| 23 | `printer.print.cancel` | cancel | 1 | Cancels the running print. |

## Notifications pushed by the backend

| # | Notification | First seen | What it carries |
|---|---|---|---|
| 1 | `notify_proc_stat_update` | page load | Host stats, about once a second. Cosmetic. |
| 2 | `notify_status_update` | page load | Deltas for subscribed objects: temperatures, position, print progress. |
| 3 | `notify_filelist_changed` | upload | A file was added; Mainsail updates the list without re-listing. |
| 4 | `notify_gcode_response` | start print | Console lines from the printer. |
| 5 | `notify_history_changed` | start print | Job added/finished; triggers `server.history.totals`. |

## HTTP requests

| # | Request | First seen | Status | What it is for |
|---|---|---|---|---|
| 1 | `GET /server/files/config/.theme/default.json`, `…/maintenance.json` | page load | 404 | Optional custom theme; 404 is fine. |
| 2 | `POST /server/database/item` | page load | 200 | Writes `mainsail.initVersion`. |
| 3 | `POST /server/files/upload` | upload | 200 | File upload (multipart); also used by "Upload & Print". |
| 4 | `GET /server/database/list` | settings | 200 / 304 | Settings dialog reads namespaces over HTTP. |
| 5 | `GET /server/database/item?namespace=mainsail` | settings | 200 / 304 | Settings dialog reads settings over HTTP. |

---

## Against the planned list

**Called, not in the plan:** `server.webcams.list`, `server.gcode_store`,
`server.temperature_store`, `server.history.list` / `.totals`, `machine.system_info`,
`machine.proc_stats`, `machine.update.status`, `server.database.*`, the HTTP routes above, and
batch support.

**In the plan, not called in this session:** `printer.gcode.help` (Mainsail uses the
`gcode.commands` object instead); object `display_status`.

**Not exercised, so still unmeasured:**
- `printer.print.pause` / `.resume`: Mainsail only shows cancel during a print after pause
  unless "Display CANCEL_PRINT" is on, and Marlinraker's pause macro moves the toolhead
  (`G1 Z5`, `G1 X0 Y0`) on unhomed axes, so pause was skipped.
- `printer.emergency_stop`, file delete/rename/move, directory creation, history details,
  heightmap, G-code viewer file load, and the Machine page's config editor.
- **Fluidd.** Not captured.

A second session covering those, using the same proxy, is cheap if needed.
