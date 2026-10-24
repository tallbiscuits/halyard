# Decisions

Why Halyard is shaped the way it is. Each entry records what was decided, what it rules
out, and the evidence behind it — so a later session can re-open a decision knowingly
rather than by accident.

---

## D1 — Serial is the transport; USB is an optional accelerator

**Decided:** the link to the printer is a plain UART, using line numbers, checksums and
Marlin's resend mechanism for integrity.

**Alternatives rejected:** USB mass-storage emulation (Halyard pretends to be a flash
drive); USB-CDC host (Halyard drives the printer's USB device port).

**Why:** a grep of Marlin's pins directory shows `HAS_OTG_USB_HOST_SUPPORT` declared by
roughly a dozen board families — SKR 2, SKR Pro, SKR SE BX, GTR, Octopus, Octopus Pro, MKS
Robin Nano V3, MKS Monster8, Lerdge X/S, Tronxy CXY-446, I3DBEEZ9 — all 32-bit STM32, out
of several hundred board definitions. Serial works on all of them and on 8-bit boards too.

Serial is also structurally more robust: no enumeration, no device class drivers, no VBUS,
nothing to re-establish when either end reboots. Marlin's USB host support is 118 lines
wrapping ST's MSC driver (`Marlin/src/HAL/STM32/sd/usb_host.cpp`), exercised by a small
fraction of users; the serial path is exercised by everyone, constantly.

**Consequence:** file hand-off is limited by baud rate (minutes, not seconds). Accepted,
and mitigated by D2 and by `BINARY_FILE_TRANSFER`.

---

## D2 — The printer prints from its own media; Halyard stores the library

**Decided:** uploads land on Halyard's SD card at WiFi speed and can arrive at any time,
including mid-print. When the printer is idle, Halyard writes the file to the printer's
own storage (`M28`) and starts it there (`M23`/`M24`).

**Alternatives rejected:** streaming G-code line by line during the print (OctoPrint's
model); storing only on the printer's card.

**Why streaming was rejected:** Marlin arms power-loss recovery only for media prints —
`feature/powerloss.cpp` gates on `card.isStillPrinting()`. Streaming also makes Halyard a
lifeline for the entire print, and can starve the motion planner on fast small-segment
G-code, which is the origin of the folk wisdom that SD prints are smoother.

**Why printer-only storage was rejected:** Marlin has no command to read a file back, so
thumbnails and metadata would be unavailable; and uploads could not happen during a print.

**Consequence:** the file exists twice, and there is a hand-off delay at print start. The
delay is hidden by sending preheat commands before the transfer.

---

## D3 — ESP32-S3 with a microSD slot

**Decided:** dual-core ESP32-S3, microSD card, 8 MB flash or more.

**Why:** two cores keep the radio and HTTP work off the printer-facing task. The SD card
is what makes D2's local library possible — and with an SD present the web UI can live on
the card, so large flash is convenient rather than required.

**Rejected:** ESP8266 (too little RAM for a real UI), ESP32-CAM (classic ESP32, 4 MB
flash), single-core C3/C6 (workable, less headroom, less prior art).

---

## D4 — Emulate OctoPrint for slicers, a Moonraker subset for the UI

**Decided:** two HTTP endpoints in OctoPrint's shape for slicer upload; a subset of
Moonraker's JSON-RPC for Mainsail/Fluidd.

**Why:** the OctoPrint upload contract is two endpoints and is supported by every slicer.
Moonraker's is larger but buys a finished, good-looking UI for free. Doing both costs
little, because they sit on the same delivery layer.

**Scope control:** [Marlinraker](https://github.com/phm07/marlinraker) is a working
Moonraker emulator for Marlin (Node.js — cannot run on the ESP, but readable as a
specification: 52 API methods, 16 faked printer objects). The API-capture proxy (`tools/api-capture/`) logs what Mainsail
actually calls so the subset is measured, not guessed.

---

## D5 — Capabilities are discovered, not configured

**Decided:** on connect, probe `M115` and adapt.

**Why:** Marlin reports `SD_WRITE`, `BINARY_FILE_TRANSFER`, `LONG_FILENAME`,
`EXTENDED_M20`, `AUTOREPORT_SD_STATUS`, `AUTOREPORT_TEMP`, `EMERGENCY_PARSER`,
`MULTI_VOLUME`, `PRINT_JOB` — exactly the axes Halyard varies along. No per-board config
files, no profile database, no user setup step.

**Requirement (confirmed 2026-09-22):** Halyard must work on printers whose firmware has
**none** of the optional features. Reflashing with them makes Halyard faster and nicer, but
must never be a precondition. Every capability therefore needs a named fallback, built and
tested, not just planned:

| Capability missing | Fallback |
|---|---|
| `BINARY_FILE_TRANSFER` | ASCII `M28`/`M29` with line numbers, checksums and resend |
| `LONG_FILENAME` / `LFN_WRITE` | Halyard assigns 8.3 names on the printer's card and keeps the mapping to real names |
| `EXTENDED_M20` | Plain `M20`; Halyard's own library is the source of truth for names and dates |
| `EMERGENCY_PARSER` | `M108`/`M112`/`M524` wait in the queue: keep it shallow, and the UI must not promise an instant stop |
| `AUTOREPORT_SD_STATUS` | Poll `M27` |
| `AUTOREPORT_TEMP` | Poll `M105` |
| `HOST_ACTION_COMMANDS` / `PROMPT_SUPPORT` | No `//action:` events; state comes from polling |
| `ADVANCED_OK` (not in `M115`; recognise it from the `ok N… P… B…` form) | Plain `ok` counting, conservatively one command in flight |
| Large RX buffer / high baud (not in `M115`) | Start at the configured baud rate with conservative pacing; try faster (`M575`) only if it proves reliable |
| `POWER_LOSS_RECOVERY` | Nothing to replace; a print interrupted by power loss is lost |
| `BABYSTEPPING` | No Z offset control in Mainsail; `SET_GCODE_OFFSET` is refused |
| `LIN_ADVANCE` (not in `M115`; ask `M900`, which only reports without arguments) | No pressure advance panel; `SET_PRESSURE_ADVANCE` is refused |
| `EEPROM_SETTINGS` (`Cap:EEPROM`; off in stock Marlin) | Calibration results last until the printer restarts; no SAVE CONFIG, and `SAVE_CONFIG`/`M500` aren't offered |
| `SD_WRITE` (or no SD at all) | A live-streaming strategy (not built), with its weaker guarantees, or refuse and explain |

**Test targets:** the SKR 2 in both states. Its firmware from before the Halyard reflash
is a real minimal configuration, and the
reflashed one has every feature. Both firmware binaries are kept, so Halyard can be tested
against either.

**Stock Marlin too (2026-10-04).** The test printer keeps gaining options, so a printer with
Marlin's own defaults is tested alongside it, every time: `FakeMarlin::stock()` is Marlin
2.1.2.8's untouched `Configuration.h` and `Configuration_adv.h`, plus SD support, which
Halyard requires. Stock has hotend PID,
temperature auto-reports and keepalives, and none of the rest: no EEPROM, probe, leveling,
babystepping, Linear Advance, host actions, long names, binary transfer, SD auto-report,
`ADVANCED_OK` or emergency parser. Its first run found two bugs the full and minimal fakes
had hidden: every unknown command stalled the link for 10 s without `ADVANCED_OK`, and SAVE
CONFIG was offered on a printer without an EEPROM.

---

## D6 — Power from a 5 V header, not the WiFi header's 3.3 V

**Decided:** take 5 V from a header with real current behind it and let the ESP regulate
its own 3.3 V.

**Why:** the WiFi header's 3.3 V rail is sized for an ESP-01 (~150 mA). An ESP32 draws
~350 mA in radio bursts and browns out. On the SKR 2 the **TFT header** carries
`+5V, GND, PA9, PA10, RST` — 5 V and a UART in one connector, built to run a touchscreen.

---

## D7 — Name: Halyard

The rope that raises the mainsail; Halyard hoists Mainsail onto a printer. Survives the
project outgrowing Mainsail, since halyards raise any sail.

**Rejected:** *Remora* (perfect metaphor — a fish that rides on a marlin — but taken by
[scottalford75/Remora](https://github.com/scottalford75/Remora), LinuxCNC firmware for
STM32 printer boards since 2017, far too close a neighbourhood); *Marlinspike* (strongly
associated with Signal's founder); *Bosun*, *Pilotfish*, *Chandler* (viable runners-up).

---

## D8 — Bluetooth is for provisioning, not data

**Decided:** no BLE file transfer. BLE considered for WiFi provisioning, and
optionally as an out-of-band control channel.

**Why:** the ESP32-S3 has BLE 5 only — no Bluetooth Classic, therefore no SPP. Realistic
BLE throughput is tens of kB/s against WiFi's megabytes, and the two share one radio, so
enabling BLE degrades the very transfer it would be competing with.

---

## D9 — Hand-off and start happen only on command

**Decided (2026-09-26):** a file waiting on Halyard's card is not copied to the printer, and
not started, until someone asks. One command does both: preheat, copy if needed, start.
Another copies without starting. A hand-off is refused while the printer prints, and
Halyard does not queue the command.

**Alternatives rejected:** starting the next job as soon as the printer frees (the
original plan), and copying on its own but starting only after a
confirmation.

**Why:** when a print ends, the finished part is still on the bed. A job that starts on
its own crashes into it, and nothing in Marlin's serial protocol tells Halyard the bed has
been cleared. Copying on its own was rejected as well: nothing should
happen at the printer unless asked.

**Consequence:** no job queue. Mainsail's "start" on a Halyard file is this
command. Automatic continuation can come back later as an opt-in (a belt
printer, or ejection G-code), not as the default.

**Also decided with it: always 8.3 names on the printer's card.** Halyard picks
`STEM~N.GCO` and records it next to the file, on both firmware builds, even when `M115`
reports `LFN_WRITE`. So there is one code path, tested on both builds, and D5's fallback
is the only path. The cost: the printer's own display shows `PHONES~1.GCO`. Long names
through `LFN_WRITE` are a possible later improvement.

---

## D10 — Stay on Arduino-ESP32 2.0.17; serve with ESP-IDF's esp_http_server

> **Replaced in part by D14 (2026-10-23):** the framework is now Arduino-ESP32 3.3 on
> ESP-IDF 5.5, through `pioarduino`. `esp_http_server` stays, and so do its limits below.

**Decided (2026-09-27):** the Mainsail API stays on the framework the link and the hand-off were tested on
(`espressif32@6.12.0`: Arduino-ESP32 2.0.17, ESP-IDF 4.4). The web server is IDF's own
`esp_http_server`, whose websocket support is already compiled into that framework
(`CONFIG_HTTPD_WS_SUPPORT=1`). JSON is ArduinoJson 7, which also builds for the host tests.

**Alternatives rejected:**
- *ESPAsyncWebServer + AsyncTCP* on the same framework. A friendlier API, but a third-party
  dependency whose callbacks run in the TCP task, with their own rules about what may
  block there.
- *Arduino 3 / IDF 5*, through the community `pioarduino` platform (official PlatformIO
  stops at Arduino 2). It has newer USB-CDC code that may fix the console's flush quirk.
  But it would mean re-verifying the link and the hand-off on a new stack,
  for no feature the Mainsail API needs.

**How it runs (D3's two cores):** the printer loop (link, hand-off, service polling) runs on
core 1. The HTTP server has its own task on core 0. One recursive mutex guards everything in
`lib/halyard` and the SD card. Websocket notifications are queued by the Service and sent
from the server's task (`httpd_queue_work`), so frames to one socket never interleave.

**Limits that come with it:**
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is 1024 bytes, fixed in the precompiled framework. A
  browser request with long headers (cookies, many `sec-ch-*` headers) could exceed it.
- Chunked request bodies are not supported. Slicers and browsers send a length.
- A slow SD write while an upload holds the lock delays the printer loop by as much. The
  UART's 4 KB receive buffer covers about 160 ms at 250000 baud.

---

## D11 — Access control follows Moonraker: trusted ranges, or an API key

**Decided (2026-09-27):** Halyard is open by default, like a MainsailOS install. A client
whose address is in a trusted range needs nothing. Any other client needs the API key in an
`X-Api-Key` header. The default ranges are MainsailOS's: `10.0.0.0/8`, `127.0.0.0/8`,
`169.254.0.0/16`, `172.16.0.0/12`, `192.168.0.0/16`, `fe80::/10` and `::1/128`. Both the
ranges and the key are set on the USB console (`trust`, `apikey`) and kept in NVS. The key
is made at first boot. Every route is checked, including the websocket handshake, Mainsail's
files, uploads and firmware updates.

**Alternatives rejected:**
- *A browser password (HTTP Basic).* It would protect against people on the same network,
  which trusted ranges cannot. But it is more to test (the websocket through Basic auth),
  and Moonraker's model was chosen instead.
- *Moonraker's user login (JWT, `access.login`).* Mainsail 2.19 has no login screen. Its
  bundle makes no `access.*` calls, and an "Unauthorized" only shows a connection failure.
  Only Fluidd has a login screen.
- *Locked until a password is set.* Halyard should work out of the box.

**Why:** Mainsail can't authenticate, so anything a browser does has to pass on its
address alone. That is exactly Moonraker's `trusted_clients`, which Klipper users already
know. Slicers do have an API key field (OctoPrint's), so clients outside the ranges still
have a way in.

**Consequences:**
- Anyone on the home network can start a print, as on a MainsailOS install. To narrow it,
  use `trust set 192.168.1.20` (one PC), and give slicers elsewhere the key.
- CORS stays open (`*`). A web page on another site, opened by a trusted browser, could
  send simple requests to Halyard. Moonraker's `cors_domains` limits which origins may
  *read* replies, but not simple POSTs either. This is not addressed.
- There is no TLS. The API key crosses the network in clear text.

---

## D12 — Settings live in `halyard.cfg` on the card; the console only fixes problems

**Decided (2026-09-27):** Halyard's settings are one file, `halyard.cfg`, at the root of its
card, in Klipper's format. Mainsail edits it like `printer.cfg`: it is the only file in the
`config` root, and **SAVE & RESTART** (which sends `FIRMWARE_RESTART`) reads it again. A new
Halyard learns its WiFi network from a card prepared on a PC (`tools/make_card.py`, which
also puts Mainsail on it). NVS keeps a copy of the last settings read, so a Halyard without a
card still joins the network and says what is wrong. The console's `wifi`, `trust` and
`apikey` edit the file. They are for fixing problems, not a step in setting up.

What is in it: `[wifi]` ssid, password and hostname; `[access]` trusted ranges and the API
key (Halyard writes a new key in when the line is empty); `[printer]` binary transfer and
screen messages. Mistakes become warnings in Mainsail's notification bell and console, and
the setting keeps its default. WiFi changes restart Halyard; everything else applies at once.

Halyard also shows its address on the printer's screen (`M117`) when it joins the network, and
problems there too: no card, no WiFi set, or a network it cannot join. Not while a print is
running, and again after the printer reconnects.

**Alternatives rejected:**
- *A setup hotspot with a captive portal.* No card reader needed, and it recovers from a
  changed network by itself. But it is much more firmware (access point, captive DNS, scan
  page) and still needs another way to install Mainsail. It can be added later as a
  recovery path without changing this.
- *BLE provisioning (D8).* Needs a phone app, and does not install Mainsail either.
- *Settings in NVS only, set on the console.* That is how Halyard worked at first. It needs a USB
  cable and a terminal, which rules out most users (Windows).

**Consequences:**
- The WiFi password is plain text on the card (for preparing it on a PC), but it never
  goes over the network. Mainsail gets `********`, and a save that keeps the stars keeps the
  password. Anyone with the card, or the GEEK and a USB cable (flash encryption is off), can
  still read it.
- The card must be FAT32 (32 GB or smaller): the firmware's FAT driver has no exFAT.
- The first flash is over USB: from a browser with the web flasher, or `run.sh flash`.

---

## D13 — Halyard ships its own Mainsail: upstream plus a small patch set

**Decided (2026-09-27):** "Mainsail for Halyard" is upstream Mainsail at a pinned release
tag, plus a short series of patches kept in this repository. CI builds it into a
`mainsail.zip` that ships with each Halyard release. The card zip, the update page and the
**mainsail** row in Mainsail's Update Manager all use that build, not upstream's. Stock
Mainsail and Fluidd keep working against Halyard, since the API is still Moonraker's; they
just speak of Klipper.

**Why:** Mainsail is a Klipper UI, and some of that is fixed in its code, not its API. For
example, the printer-state panel says "Klipper reports: STARTUP", with buttons for Klipper
and Moonraker logs that Halyard doesn't have. The update manager always shows a
"System / OS-Packages" row, and the editor links `halyard.cfg` to Klipper's config
reference. A fork is also where Halyard's own features can go: hand-off progress, the
printer's card, and the capabilities `M115` found. And it closes the API risk of Mainsail
updates: Halyard ships the Mainsail it was tested with.

**Alternatives rejected:**
- *A custom stylesheet* (`config/.theme/custom.css`, which Mainsail loads). It was built
  and worked, but it replaces text by hiding it, and finds some elements by their
  position. Any Mainsail release can undo it, silently.
- *Patching Mainsail's files on Halyard at install time.* This needs decompressing and
  recompressing the bundle on the ESP32, and it breaks just as easily.
- *A hard fork.* It drifts from upstream, and merging gets harder with every release.

**Rules:**
- Each patch is small, and says why it exists.
- Moving to a new upstream tag is deliberate: re-apply the patches, run a new API capture
  (`tools/api-capture/`), and run the tests.
- The fork is GPL-3.0, like Mainsail. Its source (the upstream tag plus the patches) is in
  this repository.

**Consequences:**
- Publishing it needs the public repository and CI. It is built
  locally until then (`mainsail/run.sh`, since 2026-09-29).
- The **mainsail** row offers the fork build the firmware names (`HALYARD_MAINSAIL`), not
  upstream's latest, and the warning for untested versions is gone. It comes from
  `halyard.cfg`'s `[updates] source`, which will default to Halyard's GitHub releases.
- Then the **mainsail** row tracks Halyard's builds, and the warning goes.

---

## D14 — Arduino-ESP32 3.3 on ESP-IDF 5.5, through pioarduino

**Decided (2026-10-23):** the firmware builds with the community `pioarduino` platform,
pinned to a release (`55.03.312-1`: Arduino-ESP32 3.3.12 on ESP-IDF 5.5.5). This replaces
D10's framework half. `lib/halyard` and the web server's design are unchanged.

**Why:** IDF 4.4 reached end of life around July 2024, and its Mbed TLS 2.28 at the end of
2024. Strangers will keep a Halyard on their home network for years, and it fetches
Mainsail over HTTPS. Official PlatformIO still ships Arduino 2.0.17 on IDF 4.4, so leaving
it means `pioarduino` or dropping Arduino.

**Alternatives rejected:**
- *Arduino 4.0 on IDF 6.1* (supported until about February 2029). The research's first
  choice, but only 4.0.0-RC1 existed when the work started. Moving from 3.3 to 4.0 later is
  a smaller step than this one was.
- *Official PlatformIO with `framework = espidf`*, no Arduino. Most of `main.cpp` (WiFi,
  SD with its retries, NVS, mDNS, the USB console, the UART) would be rewritten, and the SD
  card's fault reproduced again.
- *Staying on IDF 4.4.*

**Consequences:**
- IDF 5.5 is supported until about January 2028, so the move to 4.0 (IDF 6.x) is due
  before then.
- The partition table is pinned in `firmware/partitions.csv`: Arduino's 16 MB layout (two
  6.25 MB app slots). The GEEK has 16 MB of flash, and Arduino 2.0 had used its 8 MB
  table. An OTA update can't change the table, so a GEEK on an older build is flashed over
  USB once. NVS stays at 0x9000, so its settings survive.
- The device builds with pioarduino's `-std=gnu++2b`, and the host tests with gnu++17.
  Removing `gnu++2b` with `build_unflags` made the build pull in large framework libraries
  (esp-dl and Matter, about 1 MB), so the code is kept valid in both.
- The `esp_crt_bundle.h` clash is gone (Arduino 3 has no copy of its own).
  `verifyRollbackLater` is still a weak C function in the core, and `close_fn` must still
  close the socket. The request headers are still limited to 1024 bytes in the
  precompiled framework.

