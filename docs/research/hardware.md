# Hardware notes

## Halyard's board

**The firmware is built for the Waveshare ESP32-S3-GEEK**: its pins (the UART connector and
the microSD slot) and its 16 MB flash (the partition table, D14). Another board needs a
dual-core ESP32-S3, a microSD slot and 16 MB of flash, and a build with its own pins.

| Board | Specs (verified) | Notes |
|---|---|---|
| [Waveshare ESP32-S3-GEEK](https://www.waveshare.com/wiki/ESP32-S3-GEEK) | 16 MB flash, 2 MB PSRAM, microSD, 3-pin UART connector, 1.14" LCD, USB-A male | **The board Halyard runs on.** Power needs a USB-A socket wired to 5 V. |
| [ESP32-S3-DevKitC-1 N8R8](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html) | 8 MB flash, 8 MB PSRAM | Not supported: too little flash for the partition table, and it needs an SPI microSD breakout. The N16R8 variant has 16 MB. |
| [ESP32-S3-USB-OTG](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-usb-otg/index.html) | ESP32-S3-MINI-1-N8, microSD, LCD, USB host+device | Not supported (8 MB flash). Only of interest for a USB mass-storage hand-off (not built). |

Rejected: ESP8266 (RAM), ESP32-CAM (4 MB flash, classic ESP32, needs a USB-TTL adapter),
single-core C3/C6 (workable, less headroom and prior art).

## Connecting to any Marlin printer

Three wires plus power:

| Signal | Direction |
|---|---|
| printer TX | → ESP RX |
| printer RX | ← ESP TX |
| GND | ↔ GND |
| 5 V | printer → ESP (ESP regulates its own 3.3 V) |

Both sides are 3.3 V logic — no level shifter. Set `SERIAL_PORT_2` to whichever UART the
header belongs to, and `BAUDRATE_2` to match.

## SKR 2 profile (from the official pinout PDF)

Read from `Hardware/BIGTREETECH SKR 2-Pin.pdf` in
[bigtreetech/SKR-2](https://github.com/bigtreetech/SKR-2).

| Header | Pins | Use for Halyard |
|---|---|---|
| **TFT** | `+5V  GND  PA9  PA10  RST` | **Recommended.** USART1 plus 5 V with real current behind it (it drives a touchscreen). Needs `SERIAL_PORT 1` or `SERIAL_PORT_2 1`; the test printer already has `SERIAL_PORT 1` at 250000 baud. |
| WIFI | `GND 3.3V PB12–PB15 PB10 PB11 PD8 PD9 PC14` | USART3 (PD8 = board TX, PD9 = board RX). Usable for data, but **do not power an ESP32 from its 3.3 V** — that rail is sized for an ESP-01. |
| U-Disk | `GND  DP  DM  +5V` | USB **host** port. Only for a USB mass-storage hand-off (not built): Marlin can print from a USB drive here (`HAS_OTG_USB_HOST_SUPPORT` is declared in `pins_BTT_SKR_V2_0_common.h`). |
| USB | `D- D+ +5V GND` | Device port, to a PC. Not used. |
| SERVOS / RGB / I2C | carry `+5V` | Alternative 5 V taps. |

Marlin already defines, inside `#if ENABLED(WIFISUPPORT)`: `ESP_WIFI_MODULE_COM 3`,
`ESP_WIFI_MODULE_RESET_PIN PC14`, `ESP_WIFI_MODULE_GPIO0_PIN PB10`,
`ESP_WIFI_MODULE_GPIO4_PIN PB11` — but **no Marlin source consumes them** (a grep of
`Marlin/src` finds them only in `pins/pinsDebug_list.h`). They document wiring for a module
that runs its own firmware. Marlin contributes nothing to the WiFi path; it sees a G-code
serial port.

## Power

- ESP32-S3 peaks around 350 mA during radio bursts. Take 5 V from a header with headroom.
- Brownouts are the most likely first-week failure and look like random reboots. Soak-test
  for an hour before building anything on top.
- Halyard is powered by the printer, so it is off when the printer is off — no remote
  power-on. A Pi has the same limitation unless separately powered.

## Boards with USB host (for a USB mass-storage hand-off)

`HAS_OTG_USB_HOST_SUPPORT` is declared by: BTT SKR 2, SKR Pro, SKR SE BX, GTR, Octopus,
Octopus Pro, Octopus Max EZ; MKS Robin Nano V3, MKS Monster8; Lerdge X, Lerdge S; Tronxy
CXY-446; I3DBEEZ9. All 32-bit STM32 — roughly a dozen families out of several hundred
board definitions, which is why serial is the foundation (D1).

## ESP32-S3-GEEK, measured

- esptool: ESP32-S3R2 rev v0.2, 16 MB quad flash, **2 MB** embedded PSRAM (quad). Some
  third-party pinouts say N16R8; that is wrong for this board.
- The 3-pin UART connector is UART0: GPIO43 TX, GPIO44 RX. Console and flashing run over
  native USB (USB-Serial/JTAG), which leaves UART0 free for the printer.
- **ROM boot noise:** the chip ROM prints its boot log on GPIO43 on every reset. Marlin sees
  garbage, then `Unknown command` when the next newline arrives. Flush it with a newline after
  boot. The only way to silence it is the one-way eFuse `UART_PRINT_CONTROL`.
- Power from the SKR 2's **U-DISK** port works (the GEEK plugs straight in) as long as Marlin's
  `USB_FLASH_DRIVE_SUPPORT` is off. The port then only supplies 5 V.
- Link over the TFT header at 250000 baud, GEEK powered from U-DISK, printer's USB unplugged:
  - Soak 1 (8 min): 48 `M105`, 47 replies (the 48th was in flight at the save), 0 errors.
  - Soak 2 (15 min, cut short of the planned hour by choice): 90 `M105`, 88 temperature replies,
    1 garbled line, 1 silent poll, no reboots or brownouts. The single miss is the ROM noise
    above: it prefixed the first `M105`, so Marlin answered `Unknown command` instead of a
    temperature. Excluding that, every poll got an answer. The 90th was in flight at the save.
  - `M115` read cleanly through the bridge from the PC.

## ESP32-S3-GEEK microSD slot (from Waveshare's sources, mount confirmed on the board)

Waveshare's own code agrees on these pins everywhere: `ESP32-S3-GEEK_Code.zip` from the
[wiki](https://www.waveshare.com/wiki/ESP32-S3-GEEK) (Arduino `SD_Test`, `SD_LCD`,
MicroPython `boot.py`, and the IDF `usb_msc_wireless_disk` sdkconfig).

| Card pin | GPIO | SPI role |
|---|---|---|
| CLK | 36 | SCK |
| CMD | 35 | MOSI |
| D0 | 37 | MISO |
| D3 | 34 | CS |
| D1 | 33 | (SDMMC 4-bit only) |
| D2 | 38 | (SDMMC 4-bit only) |

- All of Waveshare's demos use **SPI** (HSPI). The `SD_Test` pin comment lists D1 and D2
  too, so 4-bit SDMMC may work. Halyard uses SPI at 20 MHz, as the demos do. That is far
  faster than the printer link (13 kB/s) and fast enough for WiFi uploads.
- GPIO33–37 are free on this chip because its PSRAM is quad, not octal. On an S3 with octal
  PSRAM or flash they would clash.
- Arduino's `SD.begin(cs, spi, hz, "/sd")` mounts the card into ESP-IDF's VFS. So the
  portable `Library` reaches it with plain `fopen("/sd/gcodes/…")`, exactly as the host
  tests reach a temporary directory.
- **Checked on the board (2026-09-26):** SPI at 20 MHz mounted a 16 GB SDHC card, and `sd`
  reported it. The mounted FAT partition was only 252 MB of the card's 15193 MB, so the card
  has another, larger partition. Check the card's layout before relying on its capacity.
- **Long writes fail now and then (2026-09-30).** Uploading a 6.7 MB file failed with
  `I/O error` at a random place in 8 of 14 tries, at 20 MHz and at 10 MHz. With
  `CORE_DEBUG_LEVEL=2` the driver says `token error [13] 0x33`: after a multi-block write,
  the card's `SEND_STATUS` reply is garbled. Arduino's driver (`sd_diskio.cpp`) retries
  nothing, so FatFs fails the file. Halyard now retries the driver's reads and writes
  (`sd_mount()` in `firmware/src/main.cpp`): 8 of 8 uploads then passed, 10 transfers
  retried and none failed, and the files read back byte for byte. The cause (the card, the
  wiring or the power) is not known.
