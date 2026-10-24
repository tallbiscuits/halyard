# API-capture tools

Laptop tools that measure the two contracts Halyard must satisfy: what slicers send to
OctoPrint, and what Mainsail asks of Moonraker. The results are in
`docs/research/api-contracts.md` and `docs/research/mainsail-api-log.md`. Everything runs from
a local venv or Docker; nothing is installed on the host.

| Command | What it does | Port |
|---|---|---|
| `./run.sh` | OctoPrint stub (`octoprint_stub.py`): `GET /api/version`, `POST /api/files/local`. Saves uploads and a per-request log to `uploads/`. | 5000 |
| `./run.sh marlinraker [/dev/ttyACM0]` | Marlinraker v0.2.3-alpha in Docker, talking to the printer. Builds the image from `marlinraker/` on first run. | 7125 |
| `./run.sh proxy` | Downloads Mainsail v2.19.0 on first run, serves it, relays all API traffic to Marlinraker and logs it to `capture/mainsail-api.jsonl`. | 8080 |
| `./run.sh proxy --upstream URL` | The same in front of any Moonraker-shaped backend, Halyard too (`../../run.sh mainsail` uses this; `--www DIR` serves another Mainsail build). | 8080 |
| `./run.sh summarize [log]` | Deduplicates a capture into markdown tables, in first-seen order. | — |

`marlinraker/` is a clone of https://github.com/phm07/marlinraker (master, `68d35be`). Clone it
again if it's missing: `git clone https://github.com/phm07/marlinraker`.

Stop everything: Ctrl-C each tool, and `docker rm -f halyard-marlinraker` to release the serial
port.

## Slicer test

OrcaSlicer → Prepare tab → Wi-Fi/connection icon next to the printer → Host Type
`Octo/Klipper`, host `127.0.0.1:5000`, any API key → **Test**. Then slice and use
**Print plate ▾ → Send → Upload**. Look in `uploads/` for the file, `<name>.fields.json` and
`requests.jsonl`.

## Mainsail capture

1. `./run.sh marlinraker /dev/ttyACM0`, and wait for `Identified 3D Printer on Marlin …`.
2. `./run.sh proxy`, then open http://localhost:8080 (not :7125, which would bypass the log).
3. Tag each section before doing it: `curl 'localhost:8080/_mark?label=start-print'`.
4. `./run.sh summarize`.

For a true first-load capture, start clean: delete `marlinraker_files/database.json`,
`marlinraker_files/gcodes/*` and `capture/mainsail-api.jsonl`, and use a fresh browser profile.
Mainsail keeps state in both the backend database and the browser.

## Gotchas found the hard way

- **Marlinraker won't build on modern Node** (2023 code, native `serialport`/`diskusage`).
  Its own Dockerfile (node:16-alpine) works. The container runs as your UID with a bind-mounted
  `marlinraker_files/`; a named Docker volume comes up root-owned and Marlinraker crashes with
  `EACCES`.
- **No `dialout` group needed on the host.** `--device` plus `--group-add dialout` inside the
  container gets serial access. Any other one-off serial
  command can use the same trick:
  `docker run --rm --device /dev/ttyACM0 --group-add dialout --entrypoint sh halyard-marlinraker -c '…'`.
- **Mainsail 2.19 needs `server.webcams.list`**, which Marlinraker lacks. The proxy answers it
  (`SHIMS` in `api_capture_proxy.py`) and marks those entries `shim: true`.
- **Mainsail sends JSON-RPC batches** (arrays). An early version of the proxy crashed on them,
  and only the browser-to-backend direction died: notifications kept flowing, so the page looked
  alive while every click went nowhere. The proxy now logs relay failures immediately
  (`proxy-error` entries). Marlinraker itself rejects batches.
- **Marlinraker's macros move the printer.** `cancel_print` runs `G28 XY`. `pause` runs
  `G1 Z5 E-20` then `G1 X0 Y0`, even on unhomed axes. See `marlinraker_files/config/printer.toml`.
  Use a test file with no heater or motion commands (only `M117` + `G4 S5` lines).
- **Marlinraker streams prints from the host**, line by line. It does not print from the
  printer's SD card, so it tests Mainsail's API, not Marlin's SD printing or `M524` cancel.
- **Mainsail UI automation (Playwright):** the page has two file inputs. The one in `<header>`
  is **Upload & Print** and starts a print immediately; the file list's own input only uploads.
  The cancel button only shows during a print if UI-Settings → "Display CANCEL_PRINT" is on;
  otherwise you must pause first. The settings dialog ignores Escape; close it with its ✕.
  The header's gear icon is between the bell and the **power menu**, and the power menu has
  Restart / Firmware Restart / Reboot / Shutdown, so don't click header buttons by index.
