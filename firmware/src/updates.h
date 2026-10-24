// SPDX-License-Identifier: GPL-3.0-or-later
// Updates from Mainsail's Update Manager, in a background task on core 0. SOURCE is
// halyard.cfg's [updates] source, or Halyard's GitHub releases without one.
// - Halyard's Mainsail (D13): SOURCE/mainsail-<version>.zip, where the version is the one this
//   firmware ships with (HALYARD_MAINSAIL). The task checks that the source has it (Mainsail's
//   "check for updates"), installs it on request (its update button), and by itself when the
//   card has no Mainsail. The zip is streamed through ZipToTar into WebUi::Install, so nothing
//   is stored or unpacked twice.
// - The firmware: the check reads SOURCE/SHA256SUMS (the latest release's), which names the
//   release's version; an update writes its firmware.bin to the other app slot, checks it
//   against SHA256SUMS and restarts into it, on trial.
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#include "service.h"
#include "webui.h"

namespace updates {

// After the Service and WebUi exist; hooks them up. `release` is the Mainsail to fetch.
void begin(halyard::Service& svc, halyard::WebUi& ui, SemaphoreHandle_t core_lock,
           const std::string& release);
// From halyard.cfg, at boot and on every reload. A new source is tried at once if the card
// still has no Mainsail.
void set_source(const std::string& source);
void network_up();  // from the WiFi glue: the first time, checks and installs if needed

// For the placeholder page while the card has no Mainsail: what the automatic install is
// doing, as a sentence ("" before it has started).
std::string status();

}  // namespace updates
