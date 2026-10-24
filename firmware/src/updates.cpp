// SPDX-License-Identifier: GPL-3.0-or-later
#include "updates.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <memory>
#include <string>

#include "ota.h"
#include "release.h"
#include "version.h"  // written by version.py at build time
#include "web.h"
#include "zip_tar.h"

using halyard::ReleaseSums;
using halyard::Service;
using halyard::WebUi;
using halyard::ZipToTar;

namespace updates {

namespace {

Service* svc = nullptr;
WebUi* ui = nullptr;
SemaphoreHandle_t lock = nullptr;
QueueHandle_t jobs = nullptr;
enum Job : uint8_t { kCheck, kInstall, kFirmware, kFirstCheck, kAuto };
std::string release;          // "v2.19.0-halyard.1"
std::string source;           // halyard.cfg's; under the lock
std::string auto_status;      // for the placeholder page; under the lock
bool network_seen = false;
bool retrying = false;        // an automatic install failed; the task tries again later

// Halyard's releases, used when halyard.cfg sets no source. Mainsail comes from the release of
// this firmware's own version, which carries the Mainsail it names (a build between releases
// has no such tag); the firmware from the latest release.
#define RELEASES "https://github.com/tallbiscuits/halyard/releases/"
const char* kReleases = RELEASES "download/v" HALYARD_VERSION "/";
const char* kLatest = RELEASES "latest/download/";
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

void progress(const char* name, const std::string& msg, bool complete) {
  {
    Locked l;
    svc->update_progress(name, msg, complete);
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

// Where the firmware's SHA256SUMS and firmware.bin are: the source, or the latest release.
std::string firmware_base() {
  Locked l;
  if (source.empty()) return kLatest;
  return source.back() == '/' ? source : source + '/';
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
      int e = esp_http_client_get_errno(c);  // why: refused, timed out, no route
      err = "cannot reach " + host_of(url) + (e ? std::string(" (") + strerror(e) + ")" : "");
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
    if (status == 404) err += " (no " + url.substr(url.rfind('/') + 1) + " there)";
    break;
  }
  if (err.empty()) err = "too many redirects";
  esp_http_client_cleanup(c);
  return nullptr;
}

// A small text file (SHA256SUMS), whole. False with `err` set.
bool fetch_text(const std::string& url, std::string& text, std::string& err) {
  int64_t length = 0;
  esp_http_client_handle_t c = open_url(url, length, err);
  if (!c) return false;
  text.clear();
  char buf[512];
  int n;
  while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0 && text.size() < 16384) text.append(buf, n);
  bool whole = n == 0 && esp_http_client_is_complete_data_received(c);
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  if (!whole) err = "the download of " + url.substr(url.rfind('/') + 1) + " was cut off";
  return whole;
}

// The newest release's SHA256SUMS: its version, and firmware.bin's checksum.
bool fetch_sums(ReleaseSums& sums, std::string& err) {
  std::string text;
  if (!fetch_text(firmware_base() + "SHA256SUMS", text, err)) return false;
  if (halyard::parse_release_sums(text, sums)) return true;
  err = "its SHA256SUMS names no halyard-<version>.bin and firmware.bin";
  return false;
}

// Mainsail's "check for updates": does the source have this firmware's Mainsail (only the
// headers are read), and which firmware is the newest?
void check() {
  std::string ui_err, url = zip_url(ui_err);
  int64_t length = 0;
  if (!url.empty()) {
    esp_http_client_handle_t c = open_url(url, length, ui_err);
    if (c) {
      esp_http_client_close(c);
      esp_http_client_cleanup(c);
    }
  }
  ReleaseSums sums;
  std::string fw_err;
  fetch_sums(sums, fw_err);
  {
    Locked l;
    svc->update_checked(ui_err, sums.version, fw_err);
  }
  web::flush();
}

// Downloads and installs. False with `err` set if the installed Mainsail stayed as it was.
bool install(std::string& err) {
  std::string url = zip_url(err);
  if (url.empty()) return false;
  progress("mainsail", "Downloading Mainsail " + release + " from " + host_of(url), false);
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
      progress("mainsail", "Downloading and installing: " + std::to_string(shown) + "%", false);
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
    progress("mainsail", "Installed Mainsail " + release + " (" + std::to_string(zip.files()) +
                 " files). Reload the page to use it; the update page (/halyard/) can go back.",
             true);
  }
  return ok;
}

// Mainsail's update button.
void requested_install() {
  std::string err;
  if (!install(err)) progress("mainsail", "Update failed: " + err + ". The installed Mainsail is unchanged.", true);
}

std::string hex(const uint8_t* d, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += digits[d[i] >> 4];
    s += digits[d[i] & 15];
  }
  return s;
}

// The update button on Halyard's row: the newest release's firmware.bin onto the other app
// slot, checked against its SHA256SUMS, then a restart into it, on trial as from the update
// page. False with `err` set if the running firmware stays.
bool install_firmware(std::string& err) {
  ReleaseSums sums;
  if (!fetch_sums(sums, err)) return false;
  std::string url = firmware_base();
  if (url == kLatest) url = RELEASES "download/v" + sums.version + "/";  // the release checked
  url += "firmware.bin";
  progress("halyard", "Downloading Halyard " + sums.version + " from " + host_of(url), false);
  int64_t length = 0;
  esp_http_client_handle_t c = open_url(url, length, err);
  if (!c) return false;
  ota::Writer w;
  bool ok = w.begin(length > 0 ? (size_t)length : 0);
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t buf[4096];
  int64_t got = 0;
  int shown = 0, n = 0;
  while (ok && (n = esp_http_client_read(c, (char*)buf, sizeof buf)) > 0) {
    mbedtls_sha256_update(&sha, buf, n);
    ok = w.write(buf, n);
    got += n;
    int pct = length > 0 ? (int)(got * 100 / length) : 0;
    if (pct >= shown + 20 && pct < 100) {
      shown = pct - pct % 20;
      progress("halyard", "Downloading and writing: " + std::to_string(shown) + "%", false);
    }
  }
  bool whole = n == 0 && esp_http_client_is_complete_data_received(c);
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!ok) return err = w.error(), false;
  if (!whole) return err = "the download was cut off", false;
  if (hex(digest, sizeof digest) != sums.firmware_sha256)
    return err = "firmware.bin does not match its SHA256SUMS", false;
  // A hand-off may have started during the download, and a restart would lose it.
  {
    Locked l;
    if (!svc->can_restart(err)) return false;
  }
  if (!w.finish()) return err = w.error(), false;
  progress("halyard",
           "Installed Halyard " + sums.version +
               ". Halyard restarts into it now, and Mainsail reconnects by itself. The new "
               "firmware is kept once it has answered Mainsail; if it can't, Halyard goes back.",
           true);
  Locked l;
  svc->reboot_soon();
  return true;
}

void requested_firmware() {
  std::string err;
  if (!install_firmware(err)) progress("halyard", "Update failed: " + err + ". Halyard is unchanged.", true);
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
  progress("mainsail", "Automatic install failed: " + err, true);
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
    } else if (j == kFirmware) {
      requested_firmware();
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
    Job j = name == "halyard" ? kFirmware : kInstall;
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
