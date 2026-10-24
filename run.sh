#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Halyard: build, test, flash and update the firmware, and prepare its card. Idempotent.
#   ./run.sh test               host unit tests against the simulated Marlin
#   ./run.sh build              compile the GEEK firmware
#   ./run.sh flash  [device]    test, build, then flash over USB via esptool in Docker
#   ./run.sh monitor [device]   the console (Ctrl-X quits); needs a real terminal
#   ./run.sh ota     [host]     test, build, then update over WiFi (default halyard.local)
#   ./run.sh webui   [host]     put Halyard's Mainsail (mainsail/, D13) on Halyard's card
#   ./run.sh mainsail [host]    Mainsail served from the PC on http://localhost:8080, pointed
#                               at Halyard; every call is logged to capture/
#   ./run.sh card OUT [SSID [PASSWORD]]
#                               prepare a microSD card on the PC (OUT: the mounted card, or a
#                               .zip): halyard.cfg and Mainsail. tools/make_card.py does it.
#   ./run.sh release [VERSION]  test, then build what a release ships into dist/: the merged
#                               image for a first install, firmware.bin for updates, the card
#                               zip, and the web flasher (default version: 0.5.0-g<commit>)
#   ./run.sh flasher [PORT]     serve dist/ on http://localhost:PORT (8000) to try the flasher
#   ./run.sh files [PORT]       serve dist/release/ to the network (default 8000), as an update
#                               source for halyard.cfg's [updates] source until GitHub has them
#   ./run.sh wsgap [host] [--size MB | --file PATH]
#                               upload and download a large file while connected like Mainsail,
#                               and report the longest websocket silence (Mainsail gives up at
#                               10 s). Python from venv/ (tools/requirements.txt).
# From outside Halyard's trusted ranges, set HALYARD_API_KEY (the console's "apikey").
# The port is reached through Docker (--device + --group-add dialout); the host user is not
# in dialout. esptool needs the device under its real name inside the container.
set -e
cd "$(dirname "$0")"
PIO=~/.platformio/penv/bin/pio
DEV="${2:-$(ls /dev/serial/by-id/usb-Espressif_* 2>/dev/null | head -1)}"
BUILD=firmware/.pio/build/geek
BOOT_APP0=$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
# Halyard's Mainsail (D13): the upstream tag in mainsail/UPSTREAM plus its patches, built by
# mainsail/run.sh. Firmware (version.py) names the same version.
MAINSAIL_VERSION=$(cat mainsail/UPSTREAM)-halyard.$(cat mainsail/REVISION)
KEY_HDR=(); [ -n "$HALYARD_API_KEY" ] && KEY_HDR=(-H "X-Api-Key: $HALYARD_API_KEY")

serial_image() {
  docker image inspect halyard-esptool >/dev/null 2>&1 || docker build -t halyard-esptool - <<'DOCKERFILE'
FROM python:3.12-slim
RUN pip install --no-cache-dir esptool==5.4.0 pyserial==3.5
DOCKERFILE
}
halyard_version() { curl -fsS -m 5 "${KEY_HDR[@]}" "http://$1/api/version" 2>/dev/null | sed -n 's/.*(Halyard \([^)]*\)).*/\1/p'; }

MAINSAIL_DIR=webui/mainsail-$MAINSAIL_VERSION
MAINSAIL_ZIP=mainsail/dist/mainsail-$MAINSAIL_VERSION.zip  # the tests unpack it too
mainsail_release() {
  mkdir -p webui
  [ -f "$MAINSAIL_ZIP" ] || mainsail/run.sh build
  if [ ! -f "$MAINSAIL_DIR.unpacked" ] || [ "$MAINSAIL_ZIP" -nt "$MAINSAIL_DIR.unpacked" ]; then
    rm -rf "$MAINSAIL_DIR" && mkdir -p "$MAINSAIL_DIR"
    (cd "$MAINSAIL_DIR" && unzip -oq "$OLDPWD/$MAINSAIL_ZIP")
    touch "$MAINSAIL_DIR.unpacked"  # beside it: the card and the tar take the folder whole
  fi
}

# Mainsail's release, each file gzipped where that saves at least a tenth, as one ustar tar.
webui_tar() {
  local dir=$MAINSAIL_DIR stage=webui/stage
  mainsail_release
  rm -rf "$stage" && cp -r "$dir" "$stage"
  find "$stage" -type f | while read -r f; do
    gzip -9 -n -c "$f" > "$f.gz"
    if [ $(( $(stat -c%s "$f.gz") * 10 )) -lt $(( $(stat -c%s "$f") * 9 )) ]; then rm "$f"; else rm "$f.gz"; fi
  done
  tar --format=ustar --owner=0 --group=0 -C "$stage" -cf webui/mainsail.tar .
  echo "Mainsail $MAINSAIL_VERSION: $(find "$dir" -type f | wc -l) files, $(du -sh "$dir" | cut -f1) -> $(du -sh webui/mainsail.tar | cut -f1) as sent"
}

run_tests() {
  mainsail_release  # test_zip_to_tar_real_mainsail and the Node test use the fork's zip
  $PIO test -d firmware -e native
  # The update page's zip, gzip and tar code, in Node (it has the browser's streams).
  if command -v node >/dev/null; then
    node firmware/test/js/test_update_page.mjs "$MAINSAIL_ZIP" "$BUILD"
  else echo "node not found: the update page's test was skipped"; fi
}

# What a GitHub release will carry, built locally. The merged image holds everything
# "flash" writes, from 0x0, for ESP Web Tools and "esptool write-flash 0x0".
release() {
  local ver=$1 rel=dist/release fl=dist/flasher
  [[ "$ver" =~ ^[A-Za-z0-9][A-Za-z0-9.+-]*$ ]] || { echo "bad version: $ver" >&2; exit 2; }
  [ -z "$(git status --porcelain -- . 2>/dev/null)" ] || echo "note: uncommitted changes go into this build" >&2
  HALYARD_VERSION=$ver $PIO run -d firmware -e geek
  run_tests  # after the build: the update page's test checks this firmware.bin
  serial_image; mainsail_release
  rm -rf dist && mkdir -p "$rel" "$fl"
  docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/$BUILD:/fw:ro" -v "$BOOT_APP0:/boot_app0.bin:ro" \
    -v "$PWD/$rel:/out" halyard-esptool \
    esptool --chip esp32s3 merge-bin -o "/out/halyard-$ver.bin" \
      0x0 /fw/bootloader.bin 0x8000 /fw/partitions.bin 0xe000 /boot_app0.bin 0x10000 /fw/firmware.bin
  cp "$BUILD/firmware.bin" "$rel/firmware.bin"
  cp "$MAINSAIL_ZIP" "$rel/"  # the file the firmware fetches: SOURCE/mainsail-<version>.zip
  python3 tools/make_card.py "$MAINSAIL_DIR" "$rel/halyard-card-$ver.zip"
  cp "$rel/halyard-$ver.bin" "$fl/"
  sed -e "s/@VERSION@/$ver/g" -e "s|@RELEASES@|${HALYARD_RELEASES:-../release/halyard-card-$ver.zip}|g" \
    flasher/index.html > "$fl/index.html"
  cat > "$fl/manifest.json" <<JSON
{
  "name": "Halyard",
  "version": "$ver",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    { "chipFamily": "ESP32-S3", "parts": [ { "path": "halyard-$ver.bin", "offset": 0 } ] }
  ]
}
JSON
  (cd "$rel" && sha256sum -- * > SHA256SUMS)
  echo; echo "dist/ for Halyard $ver:"; (cd dist && find . -type f | sort | xargs ls -l | awk '{printf "  %9d  %s\n", $5, $NF}')
}

need_dev() {
  [ -e "$DEV" ] || { echo "no ESP32 device found (pass one as arg 2)" >&2; exit 1; }
  PORT=$(readlink -f "$DEV")
}

case "${1:-test}" in
  test)    run_tests ;;
  build)   $PIO run -d firmware -e geek ;;
  flash)   need_dev; $PIO test -d firmware -e native; $PIO run -d firmware -e geek; serial_image
           docker run --rm --device "$PORT" --group-add dialout \
             -v "$PWD/$BUILD:/fw:ro" -v "$BOOT_APP0:/boot_app0.bin:ro" halyard-esptool \
             esptool --chip esp32s3 --port "$PORT" write-flash \
               0x0 /fw/bootloader.bin 0x8000 /fw/partitions.bin 0xe000 /boot_app0.bin \
               0x10000 /fw/firmware.bin ;;
  monitor) need_dev; serial_image
           docker run --rm -it --device "$PORT" --group-add dialout halyard-esptool \
             python -m serial.tools.miniterm --eol CR --raw --exit-char 24 "$PORT" 115200 ;;
  ota)     HOST="${2:-halyard.local}"
           $PIO test -d firmware -e native; $PIO run -d firmware -e geek
           before=$(halyard_version "$HOST"); [ -n "$before" ] || { echo "Halyard not answering at $HOST" >&2; exit 1; }
           echo "running: $before; sending $(stat -c%s "$BUILD/firmware.bin") bytes"
           curl -fsS "${KEY_HDR[@]}" -H "Content-Type: application/octet-stream" \
             --data-binary "@$BUILD/firmware.bin" "http://$HOST/halyard/update" || {
               echo; echo "update refused (the reply above says why)" >&2; exit 1; }
           echo
           # The new firmware is on trial until it answers a request: this one confirms it.
           for i in $(seq 60); do
             sleep 2; now=$(halyard_version "$HOST")
             if [ -n "$now" ]; then echo "now running: $now"; [ "$now" != "$before" ] || echo "(the same version: rolled back, or nothing changed)"; exit 0; fi
           done
           echo "Halyard did not come back within 2 minutes; check the console" >&2; exit 1 ;;
  webui)   HOST="${2:-halyard.local}"; mkdir -p webui; webui_tar
           curl -fsS "${KEY_HDR[@]}" -H "Content-Type: application/x-tar" \
             --data-binary @webui/mainsail.tar "http://$HOST/halyard/webui" || {
               echo; echo "install failed (the reply above says why)" >&2; exit 1; }
           echo; echo "open http://$HOST/" ;;
  mainsail) HOST="${2:-halyard.local}"; mkdir -p capture
           # The API-capture proxy (tools/api-capture) serves Mainsail from the PC and relays the API.
           exec tools/api-capture/run.sh proxy --upstream "http://$HOST" \
             --log "$PWD/capture/halyard-api.jsonl" ;;
  card)    [ -n "$2" ] || { echo "usage: $0 card OUT [SSID [PASSWORD]]" >&2; exit 2; }
           mkdir -p webui; mainsail_release
           python3 tools/make_card.py "$MAINSAIL_DIR" "$2" ${3:+--ssid "$3"} ${4:+--password "$4"} ;;
  release) BASE=$(sed -n 's/^BASE = "\(.*\)"/\1/p' firmware/version.py)
           release "${2:-$BASE-g$(git describe --always --dirty --abbrev=7)}" ;;
  files)   [ -f "dist/release/mainsail-$MAINSAIL_VERSION.zip" ] || { echo "no dist/release: run $0 release first" >&2; exit 1; }
           ip=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
           echo "in halyard.cfg:  [updates] source: http://${ip:-THIS-PC}:${2:-8000}/   (Ctrl-C stops)"
           exec python3 -m http.server "${2:-8000}" --directory dist/release ;;
  flasher) [ -f dist/flasher/manifest.json ] || { echo "no dist/flasher: run $0 release first" >&2; exit 1; }
           echo "the flasher: http://localhost:${2:-8000}/flasher/ (Chrome or Edge; Ctrl-C stops)"
           exec python3 -m http.server "${2:-8000}" --bind 127.0.0.1 --directory dist ;;
  wsgap)   [ -d venv ] || python3 -m venv venv
           venv/bin/pip install -q -r tools/requirements.txt
           exec venv/bin/python tools/wsgap.py "${2:-halyard.local}" "${@:3}" ;;
  *)       echo "usage: $0 [test|build|flash|monitor [device] | ota|webui|mainsail|wsgap [host] | card OUT [SSID [PASSWORD]] | release [VERSION] | flasher [PORT] | files [PORT]]" >&2; exit 2 ;;
esac
