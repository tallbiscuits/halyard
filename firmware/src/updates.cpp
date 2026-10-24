// SPDX-License-Identifier: GPL-3.0-or-later
#include "updates.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <memory>
#include <string>

#include "version.h"  // written by version.py at build time
#include "web.h"
#include "zip_tar.h"

using halyard::Service;
using halyard::WebUi;
using halyard::ZipToTar;

namespace updates {

namespace {

Service* svc = nullptr;
WebUi* ui = nullptr;
SemaphoreHandle_t lock = nullptr;
QueueHandle_t jobs = nullptr;
enum Job : uint8_t { kCheck, kInstall, kFirstCheck, kAuto };
std::string release;          // "v2.19.0-halyard.1"
std::string source;           // halyard.cfg's; under the lock
std::string auto_status;      // for the placeholder page; under the lock
bool network_seen = false;
bool retrying = false;        // an automatic install failed; the task tries again later

// Halyard's releases, used when halyard.cfg sets no source: the release of this firmware's own
// version, which carries the Mainsail it names. A build between releases has no such tag.
const char* kReleases = "https://github.com/tallbiscuits/halyard/releases/download/v" HALYARD_VERSION "/";
const uint32_t kRetryMs = 10 * 60 * 1000;

struct Locked {
  Locked() { xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGiveRecursive(lock); }
};

void queue(Job j) {
  if (jobs) xQueueSend(jobs, &j, 0);
}

void set_status(const std::string& s) {
  Locked l;
  auto_status = s;
}

void progress(const std::string& msg, bool complete) {
  {
    Locked l;
    svc->update_progress("mainsail", msg, complete);
  }
  web::flush();
}

// The zip's address, or "" (with `err`) when there is no source.
std::string zip_url(std::string& err) {
  std::string base;
  {
    Locked l;
    base = source.empty() ? kReleases : source;
  }
  if (base.empty()) {
    err = "no update source: set [updates] source in halyard.cfg";
    return "";
  }
  if (base.back() != '/') base += '/';
  return base + "mainsail-" + release + ".zip";
}

std::string host_of(const std::string& url) {
  size_t s = url.find("://");
  s = s == std::string::npos ? 0 : s + 3;
  return url.substr(s, url.find('/', s) - s);
}

// A GET that follows redirects (a GitHub release download goes to another host), over http
// or https. Returns the open client with the headers read, or nullptr with `err` set.
esp_http_client_handle_t open_url(const std::string& url, int64_t& length, std::string& err) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.user_agent = "Halyard";  // GitHub refuses requests without one
  cfg.buffer_size = 4096;      // a redirect's Location can be a long signed URL
  cfg.buffer_size_tx = 1024;
  cfg.timeout_ms = 20000;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return err = "out of memory", nullptr;
  for (int hop = 0; hop < 5; hop++) {
    if (esp_http_client_open(c, 0) != ESP_OK) {
      err = "cannot reach " + host_of(url);
      break;
    }
    length = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status == 200) return c;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      esp_http_client_set_redirection(c);
      esp_http_client_close(c);
      continue;
    }
    err = host_of(url) + " answered " + std::to_string(status);
    if (status == 404) err += " (no mainsail-" + release + ".zip there)";
    break;
  }
  if (err.empty()) err = "too many redirects";
  esp_http_client_cleanup(c);
  return nullptr;
}

// Mainsail's "check for updates": does the source have this firmware's Mainsail? Only the
// headers are read.
void check() {
  std::string err, url = zip_url(err);
  int64_t length = 0;
  if (!url.empty()) {
    esp_http_client_handle_t c = open_url(url, length, err);
    if (c) {
      esp_http_client_close(c);
      esp_http_client_cleanup(c);
    }
  }
  {
    Locked l;
    svc->update_checked(err);
  }
  web::flush();
}

// Downloads and installs. False with `err` set if the installed Mainsail stayed as it was.
bool install(std::string& err) {
  std::string url = zip_url(err);
  if (url.empty()) return false;
  progress("Downloading Mainsail " + release + " from " + host_of(url), false);
  int64_t length = 0;
  esp_http_client_handle_t c = open_url(url, length, err);
  if (!c) return false;

  std::unique_ptr<WebUi::Install> in;
  {
    Locked l;
    in = ui->begin_install();
  }
  ZipToTar zip([&](const uint8_t* d, size_t n) {
    Locked l;
    return in->feed(d, n);
  });
  static uint8_t buf[4096];
  int64_t got = 0;
  int shown = 0, n = 0;
  bool ok = true;
  while (ok && (n = esp_http_client_read(c, (char*)buf, sizeof buf)) > 0) {
    ok = zip.feed(buf, n);
    got += n;
    int pct = length > 0 ? (int)(got * 100 / length) : 0;
    if (pct >= shown + 20 && pct < 100) {
      shown = pct - pct % 20;
      progress("Downloading and installing: " + std::to_string(shown) + "%", false);
    }
  }
  bool whole = n == 0 && esp_http_client_is_complete_data_received(c);
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  if (ok && !whole) err = "the download was cut off";
  ok = ok && whole && zip.finish();
  {
    Locked l;
    ok = ok && in->finish();
    if (!ok && err.empty()) err = !in->error().empty() ? in->error() : zip.error();
    in.reset();  // removes an unfinished install, on the card: under the lock
  }
  if (ok) {
    progress("Installed Mainsail " + release + " (" + std::to_string(zip.files()) +
                 " files). Reload the page to use it; the update page (/halyard/) can go back.",
             true);
  }
  return ok;
}

// Mainsail's update button.
void requested_install() {
  std::string err;
  if (!install(err)) progress("Update failed: " + err + ". The installed Mainsail is unchanged.", true);
}

// The card has none: Halyard fetches its own. Only then, so a Mainsail installed on purpose
// (the update page takes any) stays. After a failure it tries again every kRetryMs.
void automatic_install() {
  bool needed;
  {
    Locked l;
    needed = !svc->card_missing() && !ui->installed();
  }
  retrying = false;
  if (!needed) return set_status("");
  set_status("Halyard is downloading and installing Mainsail " + release +
             ". Reload this page in a minute or two.");
  std::string err;
  if (install(err)) return set_status("");
  progress("Automatic install failed: " + err, true);
  retrying = true;
  set_status("Halyard could not install Mainsail by itself: " + err +
             ". It tries again every 10 minutes.");
}

void task(void*) {
  for (;;) {
    Job j;
    TickType_t wait = retrying ? pdMS_TO_TICKS(kRetryMs) : portMAX_DELAY;
    if (xQueueReceive(jobs, &j, wait) != pdTRUE) j = kAuto;  // the retry is due
    if (j == kFirstCheck) vTaskDelay(pdMS_TO_TICKS(30000));  // clear of the first page loads
    if (j == kInstall) {
      requested_install();
    } else {
      check();
      if (j != kCheck) automatic_install();
    }
  }
}

}  // namespace

void begin(Service& s, WebUi& w, SemaphoreHandle_t core_lock, const std::string& rel) {
  svc = &s;
  ui = &w;
  lock = core_lock;
  release = rel;
  jobs = xQueueCreate(4, sizeof(Job));
  // Core 0 at the idle task's priority: a TLS handshake is seconds of arithmetic, and above
  // idle it would starve the idle task whenever the web server pauses, and the task watchdog
  // resets Halyard (found on hardware: a page load 13 s after boot, during the first check).
  // At the same priority they share the core in time slices.
  xTaskCreatePinnedToCore(task, "updates", 10240, nullptr, tskIDLE_PRIORITY, nullptr, 0);
  s.on_update_check = [] { queue(kCheck); };
  s.on_update = [](const std::string& name, std::string& err) {
    Job j = kInstall;
    if (xQueueSend(jobs, &j, 0) != pdTRUE) return err = "busy", false;
    return true;
  };
}

void set_source(const std::string& s) {
  bool changed;
  {
    Locked l;
    changed = s != source;
    source = s;
  }
  if (changed && network_seen) queue(kAuto);
}

void network_up() {
  if (network_seen || !jobs) return;
  network_seen = true;
  queue(kFirstCheck);
}

std::string status() {
  Locked l;
  return auto_status;
}

}  // namespace updates
