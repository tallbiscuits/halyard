# Halyard's firmware

The firmware runs on an ESP32-S3 with a microSD card (the reference board is a Waveshare
ESP32-S3-GEEK). It serves Mainsail from its own card, takes uploads from slicers, hands files
to the printer's card and starts them there, and updates itself over WiFi. The reasons behind
the design are in [`decisions.md`](decisions.md) (D1–D14).

## Code map

`firmware/lib/halyard/` is portable C++ with no Arduino: plain POSIX stdio, so the host tests
run it against a temporary folder and a simulated Marlin. Everything in it is driven by
`poll(now_ms)`. `firmware/src/` is the ESP32 glue around it.

| Path | What |
|---|---|
| `lib/halyard/src/link.*` | The link layer: one UART to Marlin, with line numbers, checksums, resend and `ok`-based flow control. |
| `lib/halyard/src/binary_transfer.*` | Marlin's binary file transfer (`M28 B1`), the host side. |
| `lib/halyard/src/heatshrink_encoder.*` | A streaming heatshrink (LZSS) encoder for the compressed binary transfer. |
| `lib/halyard/src/printer.*` | Printer operations on top of the link: status, file list, `M28` upload, print, pause, cancel. Adapts to what `M115` reports. |
| `lib/halyard/src/library.*` | Halyard's file library on its card, with the slicer metadata, thumbnails, and each file's name on the printer's card. |
| `lib/halyard/src/metadata.*` | Slicer metadata (PrusaSlicer, OrcaSlicer, Cura), read as a file streams onto the card. |
| `lib/halyard/src/delivery.*` | The delivery layer: hands a library file to the printer's card and starts it, only on command (D9). |
| `lib/halyard/src/service.*` | The service layer: the Moonraker subset Mainsail uses, OctoPrint's upload endpoints, and Klipper's commands in Marlin terms. |
| `lib/halyard/src/settings.*` | `halyard.cfg` (D12): parsed with warnings for mistakes, the default file, and setting one value while keeping the rest. |
| `lib/halyard/src/access.*` | Trusted address ranges (IPv4 and IPv6 CIDR) and the API key (D11). |
| `lib/halyard/src/webui.*` | Mainsail on the card: installs a tar of gzipped files and swaps it in only when complete, and maps request paths to files. |
| `lib/halyard/src/zip_tar.*` | Mainsail's release zip, as it downloads, into the tar `WebUi::Install` takes. Each deflated entry becomes `.gz` without decompressing. |
| `lib/halyard/src/zip_stream.*` | The archive `server.files.zip` promises, made as it downloads, with nothing written to the card. |
| `lib/halyard/src/multipart.*`, `gcode_line.*` | A streaming `multipart/form-data` parser for uploads, and G-code line helpers. |
| `lib/halyard/src/release.*` | Reads a release's `SHA256SUMS`: its version and `firmware.bin`'s checksum. |
| `src/main.cpp` | WiFi, NVS, loading `halyard.cfg`, the USB console, mDNS (`_octoprint._tcp` for slicers) and the OTA trial. |
| `src/web.cpp` | `esp_http_server`: the websocket, uploads, Mainsail from the card, `/halyard/*`, and the access check on every route. |
| `src/updates.*` | A background task for the Update Manager: checks for and fetches Halyard's Mainsail and firmware from `[updates] source`. |
| `src/ota.*` | Writes a `firmware.bin` to the other app slot, for the update page and the Update Manager. |
| `src/update_page.h` | The update page, `GET /halyard/`. Built into the firmware, so it works without Mainsail or a card. |
| `version.py` | Writes `src/version.h` (git-ignored): the version, the build time and the Mainsail it ships with. |
| `partitions.csv` | The 16 MB partition table (D14), with two app slots for OTA. |
| `test/test_link/fake_marlin.h` | A simulated Marlin that models the protocol edges from Marlin's source: full-featured, minimal, or stock 2.1.2.8. |
| `test/test_*/` | The host suites: `link`, `delivery`, `service`, `access` and `settings`. |
| `test/js/test_update_page.mjs` | The update page's script in Node, against the real `mainsail.zip`, a card zip and a `firmware.bin`. |

Outside `firmware/`: `card/halyard.cfg` is the file a card prepared on a PC starts with (a
test checks it equals what the firmware writes), `tools/make_card.py` prepares a card,
`flasher/` holds the web flasher's page, and `site/` the website it sits in on GitHub Pages.

The printer loop runs on core 1, and the web server and the update task on core 0.
Everything that touches `lib/halyard` or the SD card holds `core_lock` (D10).

## Commands

Run from the repository's root:

```bash
./run.sh test                # the host tests under ASan/UBSan, then the update page's tests in Node
./run.sh build               # compile the firmware
./run.sh flash [dev]         # tests + build + flash over USB (esptool in Docker)
./run.sh monitor [dev]       # the USB console; Ctrl-X quits
./run.sh ota [host]          # tests + build + update over WiFi, then check the new version
./run.sh webui [host]        # put Mainsail on Halyard's card
./run.sh mainsail [host]     # Mainsail from the PC through the API-capture proxy (tools/api-capture)
./run.sh card OUT [SSID [PASSWORD]]  # prepare a card on the PC: the mounted card, or a .zip
./run.sh release [VERSION]   # tests, then what a release ships, into dist/ (below)
./run.sh flasher [PORT]      # serve dist/ on localhost (default 8000) to try the web flasher
./run.sh site [latest]       # the website with the web flasher, in dist/site/ (below)
./run.sh files [PORT]        # serve dist/release/ to the network as an [updates] source
./run.sh wsgap [host]        # a large upload and download, reporting the longest websocket silence
```

The default host is `halyard.local`. From outside the trusted ranges, set `HALYARD_API_KEY`.

## Settings: `halyard.cfg` (D12)

Settings are in `halyard.cfg` at the card's root, in Klipper's format:
- `[wifi]`: ssid, password, hostname
- `[access]`: trusted, api_key
- `[printer]`: binary_transfer, screen_messages
- `[updates]`: source (where `mainsail-<version>.zip`, `SHA256SUMS` and `firmware.bin` are;
  empty means Halyard's releases)

Mainsail lists it under **Machine** (it is the only writable file in the `config` root), and
**SAVE & RESTART** reads it again: Mainsail sends `FIRMWARE_RESTART` for a `.cfg` file, and
Halyard reads the file before reconnecting to the printer, also with the printer off (then
`FIRMWARE_RESTART` and `RESTART` are the only commands it takes). Mainsail's restart buttons
do the same. WiFi changes restart Halyard (not
during a hand-off); the rest applies at once. Mistakes go to Mainsail's console and, as
Moonraker warnings, to its notification bell. The setting then keeps its default.

- **A new card:** `./run.sh card /media/…/CARD MyNetwork password` writes `halyard.cfg` and
  Mainsail. Or unzip the card zip and fill in `[wifi]` in a text editor.
- **A card without `halyard.cfg`:** Halyard writes one from what it last ran with (NVS),
  so an existing Halyard keeps its network and key.
- **An empty `api_key`:** Halyard makes a key and writes it into the file.
- **NVS keeps a copy** of the last settings read. Without a card, Halyard still joins the
  network, and Mainsail (served from a PC) shows the missing-card warning.
- **The console** (`wifi`, `trust`, `apikey`) edits the file, or NVS when there is no card.
  `config` reads the file again and lists its problems.

**The WiFi password never goes over the network.** Mainsail gets `password: ********`.
Saving with the stars unchanged keeps the password on the card, and typing a new one changes
it. The file on the card is plain text, for preparing it on a PC.

**On the printer's screen** (`M117`): `Halyard 192.168.1.23` once it has an address. Problems
go there too: `Halyard: no SD card`, `Halyard: no WiFi set`, and `Halyard: can't join NAME`
after 30 s. They are sent only while the printer is idle, and again after it reconnects.
`screen_messages: off` stops them.

The card must be FAT32, so 32 GB or smaller: the firmware's FAT driver has no exFAT, and
bigger cards come as exFAT.

## Access (D11)

Set in `halyard.cfg`'s `[access]` section; the console commands below edit that file.

- **Trusted ranges** need nothing. The default is the private ranges, as in MainsailOS.
  `trust` shows them. `trust add 192.168.1.0/24`, `trust set 192.168.1.20, fe80::/10` and
  `trust default` change them, and `trust set none` trusts no one.
- **Everyone else** needs the API key in `X-Api-Key`. `apikey` shows it and `apikey new`
  replaces it. It is made at first boot. In OrcaSlicer or PrusaSlicer, the physical
  printer's "API Key" field sends it.
- Refused requests get a 401. For a browser page, the 401 explains how to fix it. A refused
  websocket is closed right after the handshake, and Mainsail shows a connection failure.
- `wifi off` forgets only the network, not the key and the ranges.

## Mainsail on the card

Halyard serves its own build of Mainsail (D13, built by `mainsail/run.sh`) from
`/sd/.halyard/www`. An install arrives as one ustar archive of the release's files, each
gzipped where that saves at least a tenth: 193 files, 10 MB, sent as 3.2 MB. Halyard unpacks
it to `/sd/.halyard/www.new` and swaps it in only when it is complete, keeping the one before
as `www.prev`. A cut-off or bad archive leaves the old UI in place.

Serving works as nginx does for Moonraker. A request the API does not claim is looked up
as `<path>.gz` (sent with `Content-Encoding: gzip`), then as `<path>`. A path without an
extension falls back to `index.html`, for Mainsail's own routes. `assets/` names carry a
hash, so they are cached for a year, and everything else is `no-cache`. Mainsail's
`config.json` leaves the host empty, so it talks to the host it came from.

Arduino's SD driver busy-waits, so long SD work on core 0 would starve its idle task and
trip the task watchdog. The installer pauses between files (`WebUi::on_busy`), the web
server's lock pauses after 100 ms of work, and the update task runs at idle priority.

## Mainsail's Update Manager (Machine page)

Halyard lists **mainsail** in Mainsail's Update Manager, like a Klipper install: its
`server.info` names the `update_manager` component. The row offers **Halyard's Mainsail**,
the version this firmware ships with (`HALYARD_MAINSAIL`, for example `v2.19.0-halyard.1`),
not upstream's latest. Refresh checks, in a background task on core 0, that `[updates] source`
has `mainsail-<version>.zip`, and the panel warns if it doesn't. **Update** downloads it (http
or https) and streams it through `ZipToTar` into the installer.

- **Automatic install.** If the card has no Mainsail, Halyard fetches it by itself, 30 s
  after WiFi comes up, again at once when `[updates] source` changes, and every 10 minutes
  after a failure. The placeholder page at `/` and the console's `version` say what it is
  doing. It never replaces a Mainsail that is there: by semver, `v2.19.0-halyard.1` is below
  `v2.19.0`, so the panel doesn't offer the fork over a stock one either.
- **A PC as the source:** `./run.sh release`, then `./run.sh files`, which prints the line to
  put in `halyard.cfg`.
- **Why the firmware names its Mainsail.** A new Mainsail can't damage Halyard, but it may
  call something Halyard doesn't have yet. A new upstream tag goes through the fork's steps
  (D13) before a firmware names it.
- **The previous Mainsail is kept** (`www.prev`). Mainsail has no rollback button, so the
  update page offers "Go back to Mainsail vX".
- The certificate bundle is ESP-IDF's (`esp_crt_bundle.h`).

**The halyard row** is the firmware. Its version is the running one (`v0.5.0`), and the check
(30 s after WiFi comes up, and on refresh) reads `SHA256SUMS` from `[updates] source`, or
from the latest GitHub release without one. The merged image's name in it
(`halyard-<version>.bin`) is the release's version, and Mainsail offers **Update** when that
is above the running one by semver. A build between releases (`0.5.0-g1a2b3c4`) is below its
release, so the panel offers that release.

- **Update** reads `SHA256SUMS` again, downloads that release's `firmware.bin` onto the other
  app slot (the same checks as the update page), and compares its SHA-256 with the file's.
  Only then does it switch slots and restart. The new firmware is on trial (below), and
  Mainsail reconnecting confirms it.
- It is refused while a file is being copied to the printer, and a copy started during the
  download stops the update before the restart. Mainsail's own panel also waits while the
  printer prints, although Halyard could update then.
- **Update all** (when both rows have one) updates the firmware first. The new firmware names
  its own Mainsail, which the mainsail row then offers.

## The update page (`/halyard/`)

The fallback for what the Update Manager can't do: install a file from the computer (offline,
another version, a stock Mainsail), go back to the previous Mainsail, and update Halyard when
Mainsail is missing or broken. It is built into the firmware, works from any browser with no
bash, curl or Docker, and is reached at `/halyard/` and from the front page Halyard shows
while the card has no Mainsail. Mainsail doesn't link to it, so its updates stay in the Update
Manager (until 0.5.0 a `navi.json` put it in Mainsail's sidebar; Halyard removes that file).

- **Firmware:** choose a `firmware.bin`. The page checks that it is an ESP32-S3 app image
  and refuses a merged USB-flash image (it starts with the bootloader) or another chip's
  image. `/halyard/update` checks the same things. The page then waits until Halyard's
  uptime shows it has restarted, and reports the new version, or that it came back on the
  old one. The page's requests confirm the trial image.
- **Mainsail:** choose Mainsail's `mainsail.zip`, or a Halyard card zip. The browser unzips
  it (`DecompressionStream`), gzips each file where that saves a tenth (`CompressionStream`),
  builds the ustar tar and posts it to `/halyard/webui`, exactly what `./run.sh webui` sends.
  This needs Chrome or Edge 103+, Firefox 113+ or Safari 16.4+.

In Chrome, installing Mainsail over an existing one takes about 46 s, and a firmware update
about 13 s from choosing the file to the new version answering.

## Firmware updates (OTA)

`./run.sh ota` sends `firmware.bin` to `POST /halyard/update`, which writes it to the other
app slot. `esp_ota_end` checks the image (chip, segments, SHA-256) before Halyard switches to it.

The new image boots **on trial**. The bootloader has rollback enabled, and
`verifyRollbackLater()` keeps the image pending. It is confirmed once it has answered an
authorised request over the network; `run.sh ota` sends one while waiting for the new
version. If it resets before that, the bootloader goes back to the previous image. If it
never answers within 5 minutes, it rolls back and restarts. A USB flash is never on trial.

An update is refused (409) while a file is being copied to the printer, because the copy
would be lost. It is allowed during a print, since the printer feeds itself. The console's
`version` shows the running slot and whether it is on trial.

While flash is written, the cache is off on both cores. The UART can then drop bytes from
the printer, and the link layer recovers them (resend, or a line dropped as garbled).

## Restarting while the printer prints

A restarted Halyard finds the printer mid-print. Marlin then runs Halyard's lines only after
the job's commands already in its queue, so replies can take seconds. The handshake allows
for that:

- `M110` and `M115` go out together, and the handshake completes on `M115`'s reply: the
  `FIRMWARE_NAME` line, the `Cap:` lines, then its `ok`.
- A printer that has said anything legible at this baud rate is busy, not gone. The
  handshake waits for up to 15 s of silence, and never changes the baud rate on it.
- The baud rate that last worked is kept in NVS, and the handshake starts there. At that
  rate, silence gets 8 s before a retry (a busy printer without autoreports is silent). At
  other rates it gets 1.5 s.
- Connecting mid-print, `M115`'s `ok` reports a full buffer (`B0`), so the window starts at 1.
  It grows once an `ok` reports free slots with nothing of Halyard's queued.

Halyard is ready about 5 s after an update mid-print, and Mainsail shows the job under its
library name again about 4 s later, once `M27 C` has named the file.

## Release files and the web flasher

`./run.sh release 0.6.0` runs the tests, builds the firmware with that version (`version.py`
takes `HALYARD_VERSION`; without one it is `<base>-g<commit>`), and writes `dist/`
(git-ignored):

| File | What it is for |
|---|---|
| `release/halyard-<ver>.bin` | The merged image for a first install: bootloader, partition table, `boot_app0` and the app, from `0x0` (`esptool merge-bin`). The same bytes as `./run.sh flash`. |
| `release/firmware.bin` | Updates: the Update Manager, the update page, `run.sh ota`. |
| `release/mainsail-<version>.zip` | The Mainsail the firmware fetches from `[updates] source`. |
| `release/halyard-card-<ver>.zip` | The card's contents (`tools/make_card.py`). |
| `release/SHA256SUMS` | Checksums of the above. The Update Manager reads the version from it (`halyard-<ver>.bin`) and checks `firmware.bin` against it. |
| `flasher/` | `index.html` (from `flasher/`), `manifest.json` and the merged image, for ESP Web Tools 10.4.0. |

`./run.sh flasher` serves `dist/`, with the flasher at `http://localhost:8000/flasher/`, as
on the website. Web Serial needs https or localhost, and Chrome or Edge. The manifest offers
to erase on a new install, skips the Improv wait (Halyard has no Improv) and has no
`serialType`, so the one build matches whatever type the browser reports for the port. The
card link points at `../release/`; set `HALYARD_RELEASES` to point it elsewhere (the release
workflow points it at the zip on the GitHub release).

### The website

`site/` is the website on GitHub Pages, <https://tallbiscuits.github.io/halyard/>: one
hand-written `index.html`, a stylesheet, the illustrations and the fonts (self-hosted, under
the SIL Open Font License), with no build step and no framework. The web flasher sits beside it
at `/flasher/`. A Pages deployment replaces the whole site, so the two are always deployed
together. `./run.sh site` writes `dist/site/`: `site/` with `@VERSION@` filled in, and the
flasher folder at `flasher/`. `tools/check_site.py` then checks that every internal link and
anchor resolves and that no placeholder is left. Try it with
`python3 -m http.server -d dist/site`.

- `./run.sh site` takes the flasher from `dist/flasher` (run `./run.sh release` first). The
  release workflow (`release.yml`, on a version tag) does that, and deploys it.
- `./run.sh site latest` builds no firmware: it downloads the latest GitHub release's merged
  image with `gh`, checks it against that release's `SHA256SUMS`, and points the card link at
  that release's card zip. `site.yml` runs it on a push to `main` that changes the site, or by
  hand, and deploys it. Without a release it fails.
- Both workflows deploy in the `pages` concurrency group, one at a time.

## Klipper's commands in Marlin terms

Mainsail's buttons send Klipper commands. Halyard translates the ones Marlin has an
equivalent for, and leaves the rest out so Halyard's Mainsail (patches 0007 and 0008) hides
them:

| Mainsail | What the printer gets | Without it |
|---|---|---|
| Jog and extrude buttons (`SAVE_GCODE_STATE`, moves, `RESTORE_GCODE_STATE`) | the moves, then `G90` and `M82` | (always there) |
| Z offset (`SET_GCODE_OFFSET Z_ADJUST=`) | `M290 P0 Z…` (babysteps) | no `Cap:BABYSTEPPING`: the control is hidden |
| Z offset's save (`Z_OFFSET_APPLY_PROBE`) | `M851 Z` = old + babysteps; SAVE CONFIG keeps it (`M500`) | no `Cap:Z_PROBE`: no save button |
| Pressure advance (`SET_PRESSURE_ADVANCE ADVANCE=`) | `M900 K…` (Linear Advance) | no answer to `M900`: the panel is hidden |
| Limits: velocity (`SET_VELOCITY_LIMIT VELOCITY=`) | `M203 X… Y…` (maximum feedrate) | no `M203` and `M204` reports: the panel is hidden |
| Limits: acceleration (`SET_VELOCITY_LIMIT ACCEL=`) | `M204 P… T…` (printing and travel) | as above |
| Limits: square corner velocity, accel to decel, cruise ratio | nothing: Marlin has none | always hidden |
| Endstops' sync button (`printer.query_endstops.status`) | `M119`, one line per switch | (always there: every Marlin has `M119`) |
| Speed factor slider | `M220 S…` (Mainsail sends it as it is) | no `M220` report: the slider stays at 100 % |
| Flow factor slider | `M221 S…` | no `M221` report: as above |
| Part fan | nothing: no `fan` object, so no slider (see below) | always hidden |
| `BED_PID_TUNE` (TARGET=60) | `M303 E-1 S60 C8 U1` | `Bad heater id` becomes the error |
| `EXTRUDER_PID_TUNE` (TARGET=210) | `M303 E0 S210 C8 U1` | |
| `CALIBRATE_Z_OFFSET` | `G28`, `G30` at the bed's centre, then Mainsail's manual-probe dialog | no `Cap:Z_PROBE`: not listed |

- **Homed axes.** Marlin reports neither which axes are homed nor its position unasked.
  Halyard counts an axis homed after a `G28` it sent (the ones named, or all), and all of
  them during a print, since a job homes first. It forgets them, as Klipper does, whenever
  the steppers may have gone off and been moved by hand: `M84`/`M18` (the axes named, or
  all; `S` only sets the timeout), the end of a print (`SD_FINISHED_RELEASECOMMAND`, or
  Halyard's `M84` after a cancel), and Marlin's silent idle timeout (`DISABLE_IDLE_*`): 120 s
  after the last move's `ok`, Marlin's default, or what `M84 S` set (`S0`: never). `M109`
  and `M190` keep the steppers on while they wait. A new connection forgets them too: the
  printer may have restarted. Homing from the printer's own menu is not seen, so press Home
  in Mainsail once.
- **Moves on an unhomed axis are refused** (`Must home axis first`), as Klipper refuses
  them: Marlin's software endstops apply only to homed axes, so a jog runs into the frame.
  `G0`/`G1` need the axes they name, `G2`/`G3`/`G5` X and Y as well; the extruder needs
  nothing. The whole console script is refused before any of it is sent (its lines go out
  together, so a `G28` in it counts for the lines after it, and the jog script is not left
  in `G91`). The fork greys out the bars layout's jog buttons too, as Mainsail's cross and
  circle layouts and Move to already do.
- **Position:** `M114` every 5 s while idle and after each console command, never during a
  print (the reply would wait behind the job's queued moves).
- **Jogging and extruding are refused during a print, paused too.** Marlin can't save the
  job's `G90`/`G91` and `M82`/`M83`, and restoring the defaults could ruin it. Change
  filament with `M600`.
- **Babysteps** move the nozzle at once, mid-print too, and only on a homed axis (Marlin
  ignores them otherwise, so Halyard refuses). `P0` keeps them out of the probe's offset on
  firmware built with `BABYSTEP_ZPROBE_OFFSET`, so every Marlin behaves alike: they last until
  the printer restarts, unless saved. Halyard never sends a bare `M290`: without any of its
  report options, Marlin answers `echo:` with no line end, and the `ok` after it is lost.
- **Linear Advance** has no `M115` capability, so Halyard asks `M900` (no arguments only
  reports) on connecting, and again after each print, since slicers write `M900 K` into
  their G-code. K means the same as Klipper's pressure advance (mm of filament per mm/s).
  Marlin has no smooth time. The reply stays out of the console.
- **Limits** are read from the reports of `M201`, `M203` and `M204` without parameters
  (Marlin 2.1 has them), on connecting, after each print (slicers write `M203`/`M204`),
  and after the console sends `M201`, `M203`, `M204`, `M501` or `M502`. The panel shows the
  lower of X and Y and the printing acceleration; its defaults are the values when the
  printer connected. Marlin's limits are per axis where Klipper's are the toolhead's.
  `M201`'s per-axis maximum holds `M204` down, and `LIMITED_MAX_FR_EDITING` holds `M203`
  down; Halyard says so in the console. As in Klipper, a change lasts until the printer
  restarts, but any `M500` (SAVE CONFIG after a calibration, say) keeps it too.
- **Square corner velocity is left out on purpose.** Marlin's nearest equivalent, junction
  deviation (`M205 J`), is a distance: J = SCV² · (√2 − 1) / acceleration, so it would have
  to be recomputed at every acceleration change, and firmware built with `CLASSIC_JERK` has
  no J at all.
- **Endstops** come from `M119` under Marlin's names (`x_min`, `z_max`, `z_probe`), which
  Mainsail shows in capitals, where Klipper gives its steppers' (`x`, `z`). The probe is
  one of them, since Halyard has no `QUERY_PROBE`. A filament sensor and `probe_en` are left
  out: they are not endstops. With a BLTouch, Marlin reads the probe in switch mode, so
  `z_probe` is TRIGGERED while the pin is stowed and open while it is deployed. The report
  stays out of the console; during a print it waits behind the job's queued lines.
- **Speed and flow** are read from the reports of `M220` and `M221` without parameters
  (`FR:150%`, `echo:E0 Flow: 95%`, the first extruder's): on connecting, after each
  `M220`/`M221` from Mainsail or the console, after each print, and every 10 s, printing
  too, since the printer's Tune menu changes them. During a print the reply waits behind the
  job's queued lines, so a change there shows a few seconds late. The replies stay out of the
  console.
- **There is no part fan slider, on purpose.** Marlin can't report the fan's speed: no
  command asks for the PWM of `M106`. During a print the job's own `M106` lines set it, and
  the printer reads those from its card, so Halyard never sees them. A slider could only show
  what Halyard last sent, which is wrong in almost every print. Set it in the console instead:
  `M106 S255` (0–255), `M107` off.

### Calibration

- **Nothing is saved on its own.** As in Klipper, a result is in use at once, and Mainsail's
  SAVE CONFIG button (`SAVE_CONFIG`, i.e. `M500`) keeps it. Without `Cap:EEPROM`, a result
  lasts until the printer restarts, the console says so, and SAVE CONFIG is not offered.
- **PID:** the link sends `M303` alone (see `research/marlin-reference.md`, "M303 goes
  alone"). `PID_CALIBRATE HEATER= TARGET=` also works.
- **Z offset:** this is Marlin's `PROBE_OFFSET_WIZARD`, driven from Mainsail. The nozzle
  goes over the probed spot at the height the probe triggered at, which is above the bed
  whatever the old offset was. Soft endstops are off meanwhile, because the nozzle may need
  to go below Z0. `TESTZ` moves it (`+`/`-` bisect, as in Klipper). `ACCEPT` sets
  `M851 Z = old + Z − G30's measurement`, homes Z again, and turns soft endstops and bed
  leveling back on. `ABORT` lifts the nozzle and changes nothing. A print can't start while
  a calibration runs.

## Heater power, history and pauses

- **Heater power:** every temperature line already carries each heater's soft PWM (`@:`
  and `B@:`, 0 to 127). It is `extruder.power` and `heater_bed.power` (0 to 1), and the
  temperature store's `powers`, so Mainsail's power column and the chart's power lines work.
- **History:** a job keeps its file's metadata from when it began, as Moonraker does
  (`size`, `modified`, slicer, estimate, filament, thumbnails), so Mainsail's History shows
  the thumbnail and the slicer's figures, even after the file is deleted. A completed job's
  `filament_used` is the slicer's `filament_total`; Halyard doesn't count extrusion as it
  prints, so a cancelled job shows 0. History lives in memory, so a restart forgets it.
- **No live tracking in the G-code viewer** (patch 0012): the printer's position is in its
  own copy, without comments, so it doesn't point into the file the viewer loaded.
- **A pause on the printer's screen:** Marlin reports a paused media print as printing
  (`M27` says `SD printing byte` while paused). With `HOST_ACTION_COMMANDS`, `M25`
  sends `//action:pause` and `M24` sends `//action:resume`, including `M25`/`M24` from the
  printer's menu, and `M600`'s `paused`/`resumed` count too. Halyard follows those lines.
  Without host actions, a pause from Halyard still ends when the printer resumes it,
  because the position `M27` reports starts moving again. A pause from the printer's
  screen goes unseen then: a dwell or a heat wait also stops the position.

## The Machine page

- **Marlin as the printer's `mcu`:** Mainsail lists every printer object named `mcu`. Halyard's
  has `app` and `mcu_version` from `M115`'s `FIRMWARE_NAME` ("Marlin 2.1.2.8 (Sep 21 …)" is
  Marlin `2.1.2.8`; a `bugfix-2.1.x` build shows its branch), and `mcu_constants`: `MCU` is
  `MACHINE_TYPE`, the machine's name (`CUSTOM_MACHINE_NAME`, "3D Printer" by default; Marlin
  doesn't report its board), then `FIRMWARE_NAME` in full, `M115`'s other fields and every
  `Cap:` line, which the row's details dialog lists. Marlin reports no load, awake time or
  clock, so there is no `last_stats`, and patch 0013 hides those parts of the row.
- **The Host row** is Klipper's in Mainsail, and Halyard takes Klipper's place: `printer.info`
  has `app` "Halyard" and `software_version` Halyard's version alone. OS is what Halyard runs
  on ("ESP-IDF v5.5.5, Arduino-ESP32 3.3.12"), in `system_info.distribution`. Patch 0014
  checks Klipper's minimum version only when the app is Klipper, shows the version whole, and
  names the app in the About tooltip instead of showing Klipper's logo.
- **Halyard's temperature:** the ESP32-S3's internal sensor is `cpu_temp` in
  `machine.proc_stats`, and Halyard sends `notify_proc_stat_update` every 5 s (Moonraker:
  every second) so the Host row stays current. Without a sensor it is `null`.
- **`marlin.cfg`:** a virtual, read-only file beside `halyard.cfg` in the config root:
  `M503`'s report with its headings (`; Steps per unit:`, then `  M92 X80.00 …`), without
  `echo:`, under a comment saying when it was read and how to change a setting (its command
  on the console, then SAVE CONFIG; without `Cap:EEPROM` it says a change lasts until the
  printer restarts). It is made from the last report when it is downloaded.
  - **Read** on connecting, after a print, and after Mainsail's console sends a command that
    `M503` listed (whatever this printer reports: `M92`, `M851`, `M301`, …), or `M500`,
    `M501`, `M502`, `M303`, `G29` or `M290`. Never while a console command or a PID tune is
    running, or while a file is being written to the printer. During a print the reply waits
    behind the job's queued lines, and lines the job prints meanwhile are left out.
  - The report stays out of the console; `M503` typed on the console shows there as usual.
    Mainsail is sent `notify_filelist_changed` when the report changes.
  - Permission `r`: Mainsail's editor opens it read-only and offers no delete or rename.
  - Without `M503` (`DISABLE_M503`) there is no `marlin.cfg`.

## The printer's card and the hand-off's progress

Halyard's own API, shown by Halyard's Mainsail (patches 0009 and 0010); other UIs ignore it.

- **The `halyard` printer object** holds `handoff` (`state` idle, listing, preheating,
  uploading or starting; `filename`, the 8.3 `printer_name`, `print`, `bytes` and `total` of
  the file on Halyard's card, `elapsed` seconds of writing) and `card_version`, which changes
  with every listing of the printer's card. The status panel shows the copy with its speed
  and time left. `display_status` still carries the progress as text for other UIs.
- **Methods**, over the websocket or as `/printer/card/…` routes:

  | Method | What |
  |---|---|
  | `printer.card.list {refresh}` | The card as `M20` lists it: `name`, `size`, `long_name` and `modified` where the printer reports them, the library file it came from (`library`, and `current`: still the copy Halyard wrote) and, while it is current, that file's `metadata` as `server.files.metadata` has it, `printing`. Lists again unless `refresh` is false. During a print, the last listing with `stale` saying why. |
  | `printer.card.print {name}` | `M23` + `M24` on a file from the listing, as it is: nothing is copied. |
  | `printer.card.delete {name}` | `M30` on a file from the listing. |
  | `printer.card.copy {filename}` | A hand-off without a print: the console's `send`. `printer.print.cancel` stops it. |

- **Only while the printer is idle.** `M20` makes the printer read its card's directory, and
  `M30` closes the open file before deleting, which would take the job's own file away
  (`research/marlin-reference.md`). So the card is listed, and printed from or deleted on,
  only while the printer is idle, with no hand-off or calibration running; otherwise the call
  is refused (409) and says why. Only names from the listing reach `M23` and `M30`.
- **When it is listed:** once the printer is idle after connecting, after each hand-off, after
  a delete, and when the panel opens or its refresh button is pressed.
- **The panel** takes G-Code Files' columns, with its cells and thumbnails. The printer can't
  send a file back, so only a current copy of a library file has metadata; other files show
  `--`.
- **Files in folders, on Marlin 2.1.2 and older:** `diveToFile()` copies each folder name of
  an `M23` or `M30` path with `strncpy` and no terminator, so the printer opens the name plus
  stack garbage and answers `open failed` (`bugfix-2.1.x` uses `strlcpy`). Halyard reads the
  version from `M115`, lists such a file with `locked` saying why, and refuses to print or
  delete it. Firmware that doesn't name its Marlin version counts as having the bug.
- **Dates:** the printer has no clock, so every file it writes itself (each hand-off) is
  dated FAT's default, 2000-01-01 01:00. Halyard leaves that date out.

## Stock Marlin

Halyard must work on stock firmware with none of the optional features (D5), so it is tested
against stock Marlin 2.1.2.8 as well: `FakeMarlin::stock()` follows the untouched
configuration (plus SD support, which Halyard requires). The link, delivery and service
suites run their main scenarios against it: handshake, uploads with line noise, hand-off and
print, pause, resume and cancel, PID, reconnecting mid-print, heater power, and what Mainsail
is shown.

## Limitations

- **`halyard.local` does not resolve in Flatpak browsers.** The sandbox resolves names
  through systemd-resolved, not the host's Avahi (`nss-mdns`). Use the IP address (the
  console's `wifi` shows it), or turn on `MulticastDNS=resolve` in systemd-resolved.
- **No TLS, and CORS stays open** (D11). The API key crosses the network in clear text.
- **Header size:** the precompiled framework takes at most 1024 bytes of request headers
  (a 431 beyond that).
- **Rollback is on trial time only.** An image that serves one request and then fails
  later is kept. Use `./run.sh flash` over USB to recover.
- **History lives in memory**, so a restart forgets it.
