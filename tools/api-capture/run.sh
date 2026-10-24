#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The API-capture tools. Idempotent: creates the venv on first run, then starts the chosen tool.
#   ./run.sh            OctoPrint stub for slicer uploads, port 5000
#   ./run.sh proxy      Mainsail + API-capture proxy in front of Marlinraker, port 8080
#   ./run.sh summarize  Deduplicate the capture log into markdown
#   ./run.sh marlinraker [/dev/ttyACM0]   Marlinraker in Docker on port 7125
set -e
cd "$(dirname "$0")"
[ ! -d venv ] && python3 -m venv venv
venv/bin/pip install -q -r requirements.txt

case "${1:-stub}" in
  stub)      shift || true; exec venv/bin/python octoprint_stub.py "$@" ;;
  proxy)     shift
             if [ ! -f mainsail/index.html ]; then
               curl -sSL -o mainsail.zip \
                 https://github.com/mainsail-crew/mainsail/releases/download/v2.19.0/mainsail.zip
               mkdir -p mainsail && (cd mainsail && unzip -oq ../mainsail.zip) && rm mainsail.zip
             fi
             exec venv/bin/python api_capture_proxy.py "$@" ;;
  summarize) shift; exec venv/bin/python summarize_capture.py "$@" ;;
  marlinraker)
             # Marlinraker in Docker (its own Dockerfile, node 16) -- no host Node deps, and
             # the container reaches the serial port without joining the dialout group.
             DEV="${2:-/dev/ttyACM0}"
             docker image inspect halyard-marlinraker >/dev/null 2>&1 \
               || docker build -t halyard-marlinraker marlinraker
             docker rm -f halyard-marlinraker >/dev/null 2>&1 || true
             DEVICE_ARGS=()
             [ -e "$DEV" ] && DEVICE_ARGS=(--device "$DEV:/dev/ttyACM0" --group-add dialout) \
               || echo "warning: $DEV not present, starting without a printer" >&2
             mkdir -p marlinraker_files
             exec docker run --rm --name halyard-marlinraker -p 7125:7125 "${DEVICE_ARGS[@]}" \
               --user "$(id -u):$(id -g)" -v "$PWD/marlinraker_files:/marlinraker_files" \
               halyard-marlinraker ;;
  *)         echo "usage: $0 [stub|proxy|summarize|marlinraker [device]]" >&2; exit 2 ;;
esac
