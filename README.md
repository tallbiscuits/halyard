# Halyard

A WiFi print server for Marlin 3D printers that runs on a cheap ESP32-S3 stick instead of a Raspberry Pi. Send G-code straight from your slicer, run the printer from Mainsail in your  
browser, and let the printer print from its own SD card. It is powered by the printer. No Linux, no second power supply, no cloud.

## What you need

- **A Marlin 2.x printer with an SD card slot** and a free serial port on a header. Optional features make it faster  
([below](#make-it-faster)).
- **A [Waveshare ESP32-S3-GEEK](https://www.waveshare.com/wiki/ESP32-S3-GEEK)**: a USB stick
with a microSD slot and a UART connector.
- **A microSD card of 32 GB or less, formatted FAT32.**
- **Three wires and power** from the printer's header to the GEEK (see step 3).
- **Chrome based browser** on a desktop computer, for the one-time install over USB.

## Quick start

### 1. Install the firmware (once, over USB)

1. Open the web flasher, linked from the [latest release](../../releases/latest), in your browser.
2. Plug the GEEK into the computer, press **Install** and pick its port. Let it erase the
 device.
3. When it says the installation is complete, **unplug the GEEK and plug it back in**.

No port listed? Unplug the GEEK, hold its BOOT button while plugging it back in, and try
again. On Linux, your user must be in the `dialout` group.

### 2. Prepare the card

1. Download `halyard-card-<version>.zip` from the [latest release](../../releases/latest)
 and unzip it onto the card.
2. Open `halyard.cfg` on the card in a text editor and fill in your network under `[wifi]`:
   ```ini
    [wifi]
    ssid: MyNetwork
    password: my secret password
   ```

Put the card in the GEEK.

### 3. Wire it to the printer

Halyard needs a serial port (a UART) on the printer's board, the one Marlin talks G-code
on, and 5 V to run from. With the printer switched off:

1. **Find a serial header.** In the board's pinout diagram, look for a header with pins
 labelled TX and RX (or with the processor's pin names, such as `PA9`/`PA10`). Headers
 meant for a touchscreen (often `TFT`), a WiFi module (`WIFI`, `ESP`) or a serial
 accessory (`UART`, `AUX`) usually carry one, often with 5 V and GND beside it. The
 display headers `EXP1`/`EXP2` and the board's USB port are not serial ports Halyard can
 use.
2. **Check that Marlin uses that port.** Marlin talks on the ports set in
 `Configuration.h`: `SERIAL_PORT` and `SERIAL_PORT_2` (and their `BAUDRATE`s), numbered
 by the UART behind the header (the board's pinout or Marlin's `pins_*.h` for the board
 says which). If the header's UART is in neither, enable it as `SERIAL_PORT_2` with
 `BAUDRATE_2 250000`, and rebuild and flash Marlin. The baud rate must be 250000 or
   115200. A port used by a touchscreen can't be shared: the screen comes off.
3. **Wire three signals, crossed over**, from the header to the GEEK's 3-pin UART
 connector (the pins are marked on the GEEK):
   
   | Printer header | GEEK        |
   | -------------- | ----------- |
   | TX             | RX (GPIO44) |
   | RX             | TX (GPIO43) |
   | GND            | GND         |
   

    Both sides use 3.3 V logic, so no level shifter is needed.
4. **Power the GEEK from 5 V.** It has a USB-A plug, so the simplest way is a USB-A socket
 (from a USB extension cable or a breakout) wired to a 5 V pin and GND on the board. A
 USB host port on the board works too, if Marlin's `USB_FLASH_DRIVE_SUPPORT` is off.
 **Never use a 3.3 V pin**: the 3.3 V rails on WiFi headers are sized for small modules,
 and an ESP32 browns out on them. Halyard draws up to about 350 mA in bursts.
5. **Check once more** that TX goes to RX, RX to TX, and that nothing is wired to a 12 V or
 24 V pin, then switch the printer on.

### 4. Power on

The printer's screen shows **`Halyard 192.168.x.y`**. Open that address in a browser:
that's Mainsail, running on Halyard. `http://halyard.local/` works too, where your system
resolves `.local` names.

### 5. Add Halyard to your slicer

In PrusaSlicer or OrcaSlicer, add a physical printer of type
**OctoPrint** at Halyard's address. Test the connection, then Send or Upload and Print.

Files go to Halyard's card in seconds. When you start a print, Halyard copies the file to
the printer's own card and starts it there, so the print carries on even if Halyard
restarts or loses WiFi. Nothing is copied or started without your command.

## If something is wrong

- **The printer's screen says what's wrong:** `Halyard: no SD card`, `Halyard: no WiFi set`,
or `Halyard: can't join NAME` (wrong network name or password).
- **Nothing on the screen?** Check the wiring (TX and RX crossed, a common GND), that the
header is the port Marlin's `SERIAL_PORT` names, and that the baud rate is 250000 or
  115200. 
- **The card isn't seen:** it must be FAT32 and 32 GB or smaller.
- **`halyard.local` doesn't open:** use the IP address from the printer's screen. Browsers
installed as Flatpaks can't resolve `.local` names.
- **Halyard's console:** plug the GEEK into a computer, open the web flasher and choose
**Logs &amp; Console**. `help` lists the commands, and `wifi` shows or sets the network.

## Everyday use

- **Settings** are in `halyard.cfg`. Edit it in Mainsail (**Machine**, `halyard.cfg`, then
**SAVE &amp; RESTART**) or on a computer with the card in a reader. The WiFi password shows as
`********`; saving with the stars keeps it.
- **Updates:** Mainsail's Update Manager (Machine page) has two rows: **halyard** updates the
firmware to the latest release, and **mainsail** updates Halyard's Mainsail to the one that
firmware ships with. Halyard boots new firmware on trial and goes back to the old one if it
fails. An update during a print is fine, since the printer prints from its own card
(Mainsail's own panel waits for the print to end). As a fallback, Halyard's built-in update
page, `http://<halyard>/halyard/`, takes a `firmware.bin` or a `mainsail.zip` from your
computer, works without Mainsail, and can go back to the previous Mainsail.
- **Access:** anyone on your home network (the private address ranges) can use Halyard, as
with MainsailOS. `[access]` in `halyard.cfg` narrows the ranges and holds the API key for
everyone else; enter it in the slicer's "API Key" field.

## Make it faster

Halyard works with Marlin's stock settings and uses these when the printer has them. Each
is optional:


| Marlin option                                       | What it gives                                                            |
| --------------------------------------------------- | ------------------------------------------------------------------------ |
| `BINARY_FILE_TRANSFER`                              | Copies to the printer's card about 3× faster, checksummed and compressed |
| `RX_BUFFER_SIZE 1024`, `BAUDRATE 250000`            | Faster, safer copies (the headers have no flow control)                  |
| `ADVANCED_OK`                                       | More commands in flight at once                                          |
| `LONG_FILENAME_HOST_SUPPORT`                        | Real file names on the printer's card, not just 8.3 names                |
| `EMERGENCY_PARSER`                                  | Cancel and stop act at once instead of waiting in the queue              |
| `AUTO_REPORT_TEMPERATURES`, `AUTO_REPORT_SD_STATUS` | The printer pushes status instead of being polled                        |
| `HOST_ACTION_COMMANDS`                              | Pauses from the printer's own screen show up in Mainsail                 |
| `POWER_LOSS_RECOVERY`                               | Works, because the printer prints from its own card                      |
| `EEPROM_SETTINGS`                                   | SAVE CONFIG in Mainsail keeps calibration results                        |


[`docs/decisions.md`](docs/decisions.md) (D5) lists what Halyard does without each one.

---

## In depth

### How it works

```
  slicer ──WiFi──┐
                 ├── Halyard (ESP32-S3 + microSD) ──UART──> Marlin printer
  browser ──WiFi─┘         files, web UI, API              prints from its own SD
```

- Slicers upload through OctoPrint's API.
- Mainsail is served from Halyard's own card, talking to a Moonraker subset.
- Files are handed to the printer's card (`M28`, binary when offered) and printed from
there.
- Firmware updates come over WiFi, with rollback.
- Access follows Moonraker's trusted-ranges-or-API-key model.

Three principles shape it:

1. **Serial is the foundation.** It works on every Marlin board; USB host works on about a
 dozen.
2. **The printer feeds itself.** Halyard delivers files, never the motion stream.
3. **Ask, don't configure.** `M115` tells Halyard what the printer can do, so there are no
 board profiles.

Not built yet, and both optional: handing files over as USB mass storage, and live
streaming.

### Building from source

Everything runs from the repository's root through `run.sh`, with PlatformIO for the
firmware and Docker for esptool and the Mainsail build. [`docs/firmware.md`](docs/firmware.md)
has the details, every command, and the limitations.

```bash
./run.sh test                                # the host tests
./run.sh flash                               # tests, build, flash the GEEK over USB
./run.sh card /media/…/CARD "SSID" PASSWORD  # prepare a card: halyard.cfg and Mainsail
./run.sh ota [host]                          # update over WiFi
./run.sh monitor                             # the USB console
./run.sh release [VERSION]                   # the release files and the web flasher, in dist/
./run.sh flasher                             # try the web flasher on localhost
```

### Repository layout


| Path                                | What                                                                                                                                                                         |
| ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `run.sh`                            | Test, build, flash, update and release; prepare a card                                                                                                                       |
| `firmware/`                         | **The firmware** (PlatformIO): `lib/halyard/` is portable C++, `src/` the ESP32 glue, `test/` the host tests                                                                 |
| `card/`                             | `halyard.cfg` as a new card starts with it                                                                                                                                   |
| `flasher/`                          | The web flasher's page (ESP Web Tools)                                                                                                                                       |
| `mainsail/`                         | Halyard's Mainsail: the upstream tag plus a few patches, and the script that builds it                                                                                       |
| `tools/`                            | Card preparation and a websocket stress test; `api-capture/` has the laptop tools that measured the APIs (OctoPrint stub, Marlinraker in Docker, Mainsail API-capture proxy) |
| `docs/firmware.md`                  | The firmware's design, settings, updates, and Klipper's commands in Marlin terms                                                                                             |
| `docs/decisions.md`                 | Why the architecture is the way it is (D1–D14)                                                                                                                               |
| `docs/research/api-contracts.md`    | Exact OctoPrint / Moonraker wire contracts                                                                                                                                   |
| `docs/research/mainsail-api-log.md` | Measured Mainsail API surface: what Halyard implements                                                                                                                       |
| `docs/research/marlin-reference.md` | Marlin's G-codes, serial protocol and binary transfer, as measured                                                                                                           |
| `docs/research/hardware.md`         | Boards, wiring, power, SKR 2 pinout findings                                                                                                                                 |


## License

Halyard is free software under the [GNU General Public License v3.0 or later](LICENSE), like
Marlin and Mainsail. The bundled heatshrink decoder in `firmware/test/heatshrink/` keeps
its own ISC license.