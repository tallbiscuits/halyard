// SPDX-License-Identifier: GPL-3.0-or-later
// Halyard's Mainsail (D13), fetched from the update source: SOURCE/mainsail-<version>.zip, where
// the version is the one this firmware ships with (HALYARD_MAINSAIL) and SOURCE is halyard.cfg's
// [updates] source. A background task on core 0 checks that the source answers (Mainsail's
// "check for updates"), installs on request (its update button), and installs by itself when
// the card has no Mainsail. The zip is streamed through ZipToTar into WebUi::Install, so
// nothing is stored or unpacked twice.
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
