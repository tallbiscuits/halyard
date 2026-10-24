// SPDX-License-Identifier: GPL-3.0-or-later
// The HTTP and websocket server (ESP-IDF's esp_http_server, D10) in front of the portable
// Service. The server runs in its own task on core 0; the printer loop runs on core 1. Every
// call into the Service, the Library or the SD card is made holding one lock (core_lock).
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "access.h"
#include "service.h"
#include "webui.h"

namespace web {

// Starts the server on port 80. Every route checks `access` (trusted address or API key).
// Paths the Service does not know are served from `ui` (Mainsail on the card).
bool begin(halyard::Service& svc, halyard::Access& access, halyard::WebUi& ui,
           SemaphoreHandle_t core_lock);
// True once a request from the network has been authorised and answered.
bool served_any();
// Sends what the Service has waiting (notifications, late replies) from the server's task.
// Call it after Service::poll; it returns at once.
void flush();

}  // namespace web
