# Marlin reference for Halyard

Verified against Marlin `bugfix-2.1.x` (commit `ed5863d`, 2026-09-22). Paths are relative
to the Marlin repository root.

## G-codes Halyard uses

| Code | Use | Notes |
|---|---|---|
| `M115` | capability probe | see below; the basis of portability |
| `M20 L T` | list files | `L` = long filenames, `T` = timestamps. `src/gcode/sd/M20.cpp` |
| `M21` / `M22` | mount / release media | needed after card changes |
| `M23 <file>` | select file | |
| `M24` | start / resume print | |
| `M25` | pause print | |
| `M27` | progress | reports byte position and total — feeds `virtual_sdcard.progress` |
| `M27 C` | current filename | |
| `M27 S<sec>` | auto-report progress | requires `AUTO_REPORT_SD_STATUS` |
| `M28 <file>` | begin writing to media | `M28 B1` = binary mode if supported |
| `M29` | close the file | |
| `M30 <file>` | delete a file | closes the open file first: **never during a print** (below) |
| `M524` | abort SD print | |
| `M105` | temperatures | |
| `M155 S<sec>` | auto-report temperatures | requires `AUTO_REPORT_TEMPERATURES` (on by default) |
| `M114` | position | |
| `M108` / `M112` | break / emergency stop | bypass the queue only with `EMERGENCY_PARSER` |
| `M303 E<n> S<t> C<c> U1` | PID autotune (`E-1` = bed) | see "M303 goes alone" below; `U1` applies the result |
| `M851` | probe offsets | no args: `  M851 X-33.00 Y-15.00 Z-0.90 ; (mm)` (no `echo:`) |
| `G30 X Y` | probe one point | `Bed X: … Y: … Z: <measured>`; leaves bed leveling **off** |
| `M420` / `M420 S<0/1>` | bed leveling state | reports `echo:Bed Leveling ON/OFF`; only with `Cap:LEVELING_DATA` |
| `M211` / `M211 S<0/1>` | soft endstops | no args: `  M211 S1 ; ON` plus `Min:`/`Max:` (the bed size) |
| `M500` | save settings to EEPROM | `SAVE_CONFIG` in Mainsail |

## `M115` capabilities Halyard branches on

From `src/gcode/host/M115.cpp`:

`SD_WRITE` · `BINARY_FILE_TRANSFER` · `LONG_FILENAME` · `LFN_WRITE` · `EXTENDED_M20` ·
`AUTOREPORT_SD_STATUS` · `AUTOREPORT_TEMP` · `AUTOREPORT_POS` · `EMERGENCY_PARSER` ·
`MULTI_VOLUME` · `SDCARD` · `PRINT_JOB` · `HOST_ACTION_COMMANDS` · `PROMPT_SUPPORT` ·
`CUSTOM_FIRMWARE_UPLOAD` · `BUILD_PERCENT` · `AUTOLEVEL` · `Z_PROBE` · `RUNOUT`

## Configuration flags that matter

| Flag | File | Why |
|---|---|---|
| `SERIAL_PORT_2` | `Configuration.h` | which UART Halyard is on (`1` = USART1 on SKR 2's TFT header) |
| `BAUDRATE_2` | `Configuration.h` | allows up to 1000000 |
| `RX_BUFFER_SIZE 1024` | `Configuration_adv.h` | **default is 32**; no flow control on these headers |
| `EMERGENCY_PARSER` | `Configuration_adv.h` | cancel/stop bypass the command queue |
| `LONG_FILENAME_HOST_SUPPORT` | `Configuration_adv.h` | `M20 L`, `M33` |
| `M20_TIMESTAMP_SUPPORT` | `Configuration_adv.h` | `M20 T` |
| `AUTO_REPORT_SD_STATUS` | `Configuration_adv.h` | push progress |
| `AUTO_REPORT_TEMPERATURES` | `Configuration_adv.h` | on by default |
| `BINARY_FILE_TRANSFER` | `Configuration_adv.h` | `M28 B1` |
| `POWER_LOSS_RECOVERY` | `Configuration_adv.h` | media prints only — `feature/powerloss.cpp` gates on `card.isStillPrinting()` |
| `MULTI_VOLUME` | `Configuration_adv.h` | `VOLUME_SD_ONBOARD` + `VOLUME_USB_FLASH_DRIVE` can coexist |
| `SDCARD_SORT_ALPHA` | `Configuration_adv.h` | tidy listings |

## Binary file transfer (`M28 B1`)

- Firmware side: `src/feature/binary_stream.h` / `.cpp`. Packet token `0xB5AD`,
  sync/resend, `FileTransfer::{QUERY, OPEN, CLOSE, WRITE, ABORT}`, heatshrink compression
  with a 512-byte decode buffer, 10 s transfer timeout.
- Host side reference: `buildroot/share/scripts/MarlinBinaryProtocol.py` (446 lines) and
  `upload.py`. Halyard's port: `firmware/lib/halyard/src/binary_transfer.*`.

**How it works (2.1.2.8, read from source and checked on the printer, 2026-09-26):**
- **Entering and leaving:** `M28 B1` echoes "Switching to Binary Protocol", then `ok`. From
  then on that port reads packets only, until a control CLOSE (protocol 0, type 2).
  - The payload buffer is the serial line buffer, `MAX_CMD_SIZE`: **96 bytes** on the test
    printer.
  - A SYNC (control type 1) is answered `ss<sync>,<buffer>,<version>` whatever the
    sequence number. It is the way to find out whether Marlin is in binary mode.
- **Packets:** `AD B5` token, sync, protocol<<4|type, size (LE16), then fletcher-16 over
  the header without the token (LE16). A payload is followed by fletcher-16 over the header
  and the payload.
  - Marlin answers `ok<sync>` per packet and `rs<sync>` to ask for a resend. A packet
    whose sync is expected−1 gets its `ok` again.
  - Once it has asked for a resend, Marlin silently drops out-of-sequence packets. That is
    why a go-back-N window works. The reference client waits for every `ok`; Halyard keeps
    up to 4 packets in flight.
- **Compression:** always compiled in (`BINARY_STREAM_COMPRESSION` is defined
  unconditionally). It is heatshrink with window 8 and lookahead 4, reported in
  `PFT:version:0.1.0:compression:heatshrink,8,4`. Halyard's own encoder round-trips with
  Marlin's decoder, which is copied into `firmware/test/heatshrink/`.
- **Emergency parser:** it keeps scanning bytes in binary mode, but `openFileWrite()`
  disables it and `closefile()` re-enables it. So only the packets before OPEN are exposed
  to it, and they are fixed and short.

**Traps: each one cost something on the printer.**
- **The file-transfer ABORT deletes `card.filename`, not the file being transferred.**
  `transfer_abort()` calls `removeFile(card.filename)`.
  - `card.filename` is the last file the card touched. Every `M20` entry overwrites it
    (`printListing` → `createFilename(filename, …)`), and so does every file the
    printer's own menu shows (`selectFileByIndex`).
  - **This deleted a user file on the test printer (2026-09-26).** A `pls` listed the card,
    then an upload found a stale transfer "busy", and an ABORT cleared it: it deleted
    `3DBENC~1.GCO`, the last file listed.
  - **Halyard never sends ABORT.** It closes the transfer with the file CLOSE (type 2),
    which keeps what was written, then deletes the partial file by name with `M30`.
  - **It is a Marlin bug, and it is still there upstream.** `transfer_abort()` in
    `bugfix-2.1.x` was unchanged on 2026-09-27. It also deletes `card.filename` when no
    transfer is open at all: it never checks `transfer_active`. The fix would be to
    remember the name `file_open()` used and delete that. No upstream issue was checked or
    filed.
- **The same deletion is built into Marlin's own 10 s transfer timeout.**
  `SDFileTransferProtocol::idle()` → `transfer_abort()`.
  - It only runs while the port is still in binary mode.
  - So Halyard must never sit in binary mode for 10 s without packets. On a stall it gives
    up after about 6 s and leaves binary mode, or reconnects, which does the same.
  - If Halyard dies mid-transfer and does not come back within 10 s, Marlin deletes
    `card.filename`, and nothing on the host can prevent it.
- **A transfer ends with `card.release()`: the printer's card is unmounted.** `M20` then
  says "No media", and `M23`, `M28` and `M30` silently do nothing, until `M21`. Halyard
  sends `M21` after every binary transfer, and whenever `M20` says "No media" while the
  printer is known to be idle. It never sends `M21` mid-print, because it re-initialises
  the card under the running print.
- **The binary OPEN goes through `openFileWrite()`, which also sets `flag.saving`.**
  - If binary mode is left while a transfer is open, an ASCII `M29` closes the file behind
    the transfer's back.
  - With compression on, the decoder then still holds pending bytes. The transfer's CLOSE
    fails on `card.write()` (`PFT:ioerror`) and never clears `transfer_active`, so every
    later OPEN is "busy".
  - The only exits are ABORT or the timeout (both delete `card.filename`), or a printer
    reset.
  - So after a SYNC finds Marlin stuck in binary mode, Halyard sends the file CLOSE first,
    while the file is still open, then the control CLOSE. With no transfer open, the file
    CLOSE is answered `PFT:invalid` and does nothing.

## Throughput arithmetic

| Link | Rate | 20 MB file |
|---|---|---|
| 250000 baud 8N1 | ~25 kB/s | ~14 min |
| 500000 baud | ~50 kB/s | ~7 min |
| 1000000 baud | ~100 kB/s | ~3.5 min |
| + `BINARY_FILE_TRANSFER` | measured below | ~7 min at 250000 |
| WiFi to Halyard's own SD | ~1 MB/s | seconds |

**Measured:** ASCII `M28`, numbered and checksummed, window 3 (`ADVANCED_OK`), TFT
header at 250000 baud: a 106,917-byte file in 8.1 s, **13.2 kB/s of file data** (about 19 kB/s on
the wire, 77 % of the line rate). The rest is `N<n> ` and `*<cs>` on short lines. A 20 MB file
is about 25 minutes at that rate.

**Measured, binary `M28 B1`**, same printer and link. A 1.08 MB file of
`M117 G1 X… Y… E…` lines took 23.0 s, **46.8 kB/s of file data**, against 72.8 s
(14.8 kB/s) for ASCII in the same session. It compressed 2.42× with 0 resends and a window
of 4. Real OrcaSlicer G-code, comments stripped, compresses 3.05× with the same encoder.
The link is no longer the only limit: the wire carried about 19 kB/s of packets.

`MEATPACK_ON_SERIAL_PORT_2` roughly halves ASCII G-code bytes and is a cheap alternative
to full binary transfer.

## What Marlin cannot do

- **Read a file back from media.** No G-code dumps file contents to serial. Metadata and
  thumbnails must be captured during upload, on Halyard's side. This drives D2.
- **Accept `M28` while printing.** Worse than refusing: `openFileWrite()` calls
  `abortFilePrintNow()`, so an `M28` sent during a media print **ends that print**. Check
  `M27` for "Not SD printing" first (Halyard does).
- **Recover a streamed print after power loss.** Recovery is media-prints-only.

## Serial protocol, as Marlin 2.1.2.8 implements it

Read from `src/gcode/queue.cpp`, `src/sd/cardreader.cpp` and `src/feature/e_parser.h`, then
confirmed with the simulator and the test printer. The link layer
(`firmware/lib/halyard/src/link.*`) is built on these rules.

- **`ok` comes after the command has run**, not when it is queued (`GCodeQueue::advance`). A
  `G4 S5` is answered 5 s later.
- **`ADVANCED_OK`:** `ok N<n> P<planner free> B<buffer free>`. `N` is present only if the line
  was numbered. `B` counts the running command as used, so an idle printer answers
  `B = BUFSIZE - 1`.
- **M105 prints its own `ok T:...`**, with no `N`, `P` or `B`.
- **A bad line** gives `Error:<why>, Last Line: <last_N>`, then `Resend: <last_N+1>` and a bare
  `ok`. **Marlin then empties its whole RX buffer** (`gcode_line_error`): lines sent after the
  bad one vanish without a reply, or are rejected in turn if they arrive later.
- **Repeats of line `last_N` or `last_N-1` are dropped silently**: no error and no `ok`. That
  makes retransmitting the oldest unanswered line safe.
- **The tail of a split line runs.** If noise turns a byte into a newline (`*` 0x2A and `\n`
  0x0A differ by one bit), the first half is rejected and the RX buffer emptied. The rest may
  still be on the wire. It then arrives as an unnumbered line and **runs as a command**
  (usually `echo:Unknown command: "57"`, the checksum digits), with its own `ok`. Unnumbered
  lines are never checksum-checked. In saving mode they are refused.
- **Line length:** Marlin keeps 95 characters (`MAX_CMD_SIZE 96`) and drops the rest silently,
  checksum included. That means an endless resend loop.
- **Characters that change on the way in:** `;` starts a comment, which also hides the
  checksum. `\` escapes the next character and is itself dropped. Backspace (0x08) erases.
  Any of these breaks a checksum computed over the sent bytes.
- **M28 saving mode:**
  - Each numbered line is written as the text between the first space and the first `*`
    (`CardReader::write_command`), plus `\r\n`.
  - Unnumbered lines are refused, except `M29`. A line containing `M29` anywhere, not followed
    by a digit, ends saving (`is_M29` uses `strstr`).
  - The emergency parser is **off** while saving.
  - If `M28` fails ("open failed"), the lines sent after it **run as G-code**: never send
    ahead of a confirmed "Writing to file".
- **`M29` outside saving** only clears the saving flag. Sending it at connect is harmless, and
  it closes an upload a previous connection left open.
- **`M30` closes the open file before it deletes** (`gcode/sd/M30.cpp`: `card.closefile()`,
  then `removeFile()`), whichever file it names. During a media print the open file is the
  job's, so the print loses it. Halyard sends `M30` only while the printer is idle
  (`Printer::remove` refuses otherwise). Read from 2.1.2.8's source, not tried on the printer.
- **`M20` reads the card's whole directory** from the main loop. Halyard lists the card only
  while the printer is idle, and shows the last listing during a print.
- **The emergency parser** also recognizes numbered lines (`N<n> M524*cs`). It acts on bytes as
  they arrive, even when the command queue is full.
- **A media-print abort** (`M524` → `abortSDPrinting`) happens between commands. It runs
  `queue.clear()`, so host lines already queued never get their `ok`. Then it injects
  `EVENT_GCODE_SD_ABORT`, which on the test printer is **`G28XY`: cancel homes X and Y**.
- **`M27 S<n>` auto-reports even when idle** ("Not SD printing" every n seconds). Turn it on
  only while printing.
- **Messages:**
  - `M28` answers `Writing to file: <name>`, and `M29` answers `Done saving file.`.
  - `M23` answers `File opened: <name> Size: <n>` and then `File selected`.
  - A finished print prints `Done printing file`.
  - `M20` lists `<8.3 path> <size>`, then ` 0x<date><time>` with `T` and ` <long name>` with
    `L`, between `Begin file list` and `End file list`. The date and time are FAT's, four hex
    digits each, in the printer's local time: the later of creation and last write. A file
    the printer wrote itself is dated `FAT_DEFAULT_DATE`/`_TIME`, 2000-01-01 01:00
    (`sd/SdBaseFile.h`), since Marlin has no clock (seen on the test printer).
  - `M30` answers `File deleted:<name>`, or `Deletion failed, File: <name>.`.
  - `M24` sends `//action:resume` when `HOST_ACTION_COMMANDS` is on.

### M303 goes alone (read from `gcode/temp/M303.cpp` and `module/temperature.cpp`, 2.1.2.8)

`PID_autotune()` loops for minutes without reading the serial port, reporting the heater
every 2 s (`print_heater_states()`, a ` T:… @:…` line without `W:`). "busy:" keepalives come
only with `BUSY_WHILE_HEATING` (`KEEPALIVE_STATE(NOT_BUSY)` otherwise). Then `M303` calls
`queue.flush_rx()`, which **empties the RX buffer**: any line sent behind it is lost. The
link therefore sends `M303` alone, holds everything behind it until its ok, and counts those
temperature lines as progress, so the stall probe does not give up on it. Failures are
plain lines, not `Error:`: `PID Autotune failed! Bad heater id` (no `PIDTEMPBED`),
`… Temperature too high`, `… timeout`. Success is `PID Autotune finished! …` followed by
`#define DEFAULT_[BED_]KP …` lines.

### Host lines during a media print (measured 2026-09-27)

During an SD print, Marlin's command ring buffer holds the job's own lines too. A line from
the host is read into it when a slot frees, and is answered (`ok`, and any output) only when
it runs, after the job commands queued ahead of it. With 1 s `G4` dwells in the job, the
handshake's replies came 3–5 s late. On a real print, a full planner holds things up the
same way. Consequences for the link:
- Don't infer "no printer" or "wrong baud rate" from a few seconds of silence while the
  printer may be printing. Legible autoreports (`T:`, `SD printing byte`) mean it is busy.
- Replies to lines sent while "flushing" arrive late too. Recognise replies by content
  (M115's `FIRMWARE_NAME`), not by counting `ok`s.
- `ok N… P… B0` from a busy printer means its buffer is full now, not that BUFSIZE is 0.
