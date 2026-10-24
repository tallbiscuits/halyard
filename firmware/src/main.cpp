// SPDX-License-Identifier: GPL-3.0-or-later
// Halyard on WiFi. Mainsail is served from the GEEK's card and talks
// to the Service over a websocket and HTTP; slicers upload through OctoPrint's API
// (src/web.cpp). The GEEK stores G-code on its own microSD card (the library) and hands files
// to the printer's card on command. Access follows Moonraker: trusted address ranges, or an
// API key. Firmware updates come over WiFi and boot on trial. Printer over the UART
// connector; the USB console is the hardware test tool ("help").
//
// Threads: this loop (core 1) drives the link, the hand-off and the Service; the web server
// has its own task on core 0. Both take core_lock around everything in lib/halyard and the SD.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <diskio_impl.h>
#include <sys/stat.h>

#include <atomic>
#include <memory>

#include "access.h"
#include "delivery.h"
#include "library.h"
#include "link.h"
#include "printer.h"
#include "service.h"
#include "settings.h"
#include "updates.h"
#include "version.h"  // written by version.py at build time
#include "web.h"
#include "webui.h"

using namespace halyard;

static const int UART_RX = 44;  // GEEK UART connector; printer TX
static const int UART_TX = 43;  // printer RX
// The GEEK's microSD slot, as Waveshare's demos drive it (SPI; docs/research/hardware.md).
static const int SD_SCK = 36, SD_MISO = 37, SD_MOSI = 35, SD_CS = 34;
static const uint32_t SD_HZ = 20000000;

struct UartTransport : Transport {
  size_t read(uint8_t* buf, size_t max) override {
    int n = Serial1.available();
    if (n <= 0) return 0;
    return Serial1.readBytes(buf, min((size_t)n, max));
  }
  size_t write(const uint8_t* buf, size_t len) override {
    int room = Serial1.availableForWrite();
    if (room <= 0) return 0;
    return Serial1.write(buf, min((size_t)room, len));
  }
  void set_baud(uint32_t baud) override { Serial1.updateBaudRate(baud); }
};

static UartTransport uart;
static Link link_(uart);  // not "link": that is POSIX link(2)
static Printer printer(link_);
static Library library("/sd/gcodes");
static Delivery delivery(printer, library);
static const char* kHostname = "halyard";  // until halyard.cfg is read
static const char* kConfigPath = "/sd/halyard.cfg";
static const char* kDbDir = "/sd/.halyard";
static WebUi webui("/sd/.halyard/www");
static Access access_;
static const uint32_t kTrialMs = 5 * 60 * 1000;  // an OTA image must answer a request by then
static bool sd_ok = false;

static halyard::Platform platform() {
  halyard::Platform p;
  p.wall_time = [] {
    time_t t = time(nullptr);
    return t > 1600000000 ? (double)t : 0.0;  // 0 until SNTP has set the clock
  };
  p.disk = [](uint64_t& total, uint64_t& used) {
    total = SD.totalBytes();
    used = SD.usedBytes();
    return total > 0;
  };
  p.card = [] { return sd_ok; };
  p.free_memory = [] { return (uint32_t)ESP.getFreeHeap(); };
  p.cpu_temp = [] { return temperatureRead(); };  // the ESP32-S3's internal sensor
  p.reboot = [] { ESP.restart(); };
  p.hostname = kHostname;
  p.hardware = "ESP32-S3 (Waveshare ESP32-S3-GEEK)";
  p.version = HALYARD_VERSION " built " HALYARD_BUILT;  // two builds of one commit differ
  p.os = std::string("ESP-IDF ") + esp_get_idf_version() + ", Arduino-ESP32 " +
         std::to_string(ESP_ARDUINO_VERSION_MAJOR) + "." + std::to_string(ESP_ARDUINO_VERSION_MINOR) +
         "." + std::to_string(ESP_ARDUINO_VERSION_PATCH);
  p.db_path = std::string(kDbDir) + "/database.json";
  p.config_path = kConfigPath;
  p.ui_version = [] { return webui.version(); };
  p.ui_release = HALYARD_MAINSAIL;
  p.firmware = HALYARD_VERSION;
  return p;
}
static Service service(link_, printer, delivery, library, platform());
static SemaphoreHandle_t core_lock;
static Preferences prefs;
static bool wifi_got_ip = false, mdns_started = false;
static SPIClass sd_spi(HSPI);
static bool raw = false;
static String input;

// USB CDC holds a partial packet until more data comes; flush so each line shows at once.
static void say(const char* s) {
  Serial.println(s);
  Serial.flush();
}
static void say(const String& s) { say(s.c_str()); }
static void say(const std::string& s) { say(s.c_str()); }

static void help() {
  say("# Halyard's library (its own SD card):");
  say("#   sd                card type, size and use");
  say("#   ls                list the library");
  say("#   info NAME         metadata of a library file");
  say("#   mkjob NAME [S]    write a harmless job (M117 + G4 only) of S seconds, default 60");
  say("#   mkbig NAME KB     write KB kilobytes of M117 lines that read like G1 moves, for");
  say("#                     transfer speed tests (harmless if printed: messages only)");
  say("#   rm NAME           delete a library file");
  say("# hand-off (only on command; nothing starts by itself):");
  say("#   send NAME         copy a library file to the printer's card");
  say("#   print NAME        preheat, copy if needed, start it (Mainsail sees it too)");
  say("#   abort             stop a running hand-off");
  say("#   binary on|off     use M28 B1 when the printer offers it (default on)");
  say("# the printer:");
  say("#   status            link, temperatures, job, progress, hand-off");
  say("#   pls               list the printer's card (M20)");
  say("#   pprint NAME       start a file already on the printer's card (M23 + M24)");
  say("#   prm NAME          delete a file on the printer's card (M30)");
  say("#   pause | resume    M25 | M24");
  say("#   cancel            M524 (the firmware may run EVENT_GCODE_SD_ABORT, e.g. G28XY)");
  say("#   caps | stats      what M115 reported | link counters");
  say("#   gc GCODE          send one command, show its reply");
  say("#   raw on|off        echo every line from the printer");
  say("#   reconnect         drop the link and handshake again");
  say("# settings live in halyard.cfg on the card (Mainsail: Machine); these edit it:");
  say("#   wifi              connection and address");
  say("#   wifi SSID [PASS]  set the network and restart (quote an SSID with spaces)");
  say("#   wifi off          forget the network");
  say("#   config            read halyard.cfg again (as SAVE & RESTART does), show problems");
  say("# access (Moonraker's model: trusted addresses need nothing, others the API key):");
  say("#   trust             the trusted ranges");
  say("#   trust add RANGE   trust one more, e.g. 192.168.1.0/24 or 192.168.1.20");
  say("#   trust set LIST    replace them all (comma-separated); \"trust set none\" for none");
  say("#   trust default     the private ranges, as MainsailOS has them");
  say("#   apikey [new]      show the API key for X-Api-Key (slicers), or make a new one");
  say("#   version           firmware version, OTA slot and its state");
}

static Printer::Done report(const char* what) {
  std::string w = what;
  return [w](bool ok, const std::string& msg) {
    say(std::string("# ") + w + (ok ? ": ok, " : ": FAILED, ") + msg);
  };
}

static String duration(double s) {
  if (s < 0) return "?";
  long t = (long)s;
  char buf[24];
  snprintf(buf, sizeof buf, "%ldh%02ldm", t / 3600, t / 60 % 60);
  return buf;
}

// ---- library ----------------------------------------------------------------------------

// ---- the SD card: Arduino's driver, with retries ----------------------------------------
//
// Arduino's SPI driver gives up on the first failed transfer. After a multi-block write it
// checks the card's status (SEND_STATUS), and on the test GEEK that reply comes back garbled
// now and then ("token error [13] 0x33"), at 10 MHz as at 20. FatFs then fails the file, and
// a 6.7 MB upload broke at a random place about every other time. Writing or reading the
// same sectors again is harmless, so Halyard wraps the driver's FatFs functions and retries.

DSTATUS ff_sd_initialize(uint8_t pdrv);  // Arduino's SD library (sd_diskio.cpp)
DSTATUS ff_sd_status(uint8_t pdrv);
DRESULT ff_sd_read(uint8_t pdrv, uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_ioctl(uint8_t pdrv, uint8_t cmd, void* buff);

static const int kSdTries = 4;
static std::atomic<uint32_t> sd_retries{0}, sd_failures{0};

static DRESULT sd_read_retry(uint8_t pdrv, uint8_t* buffer, DWORD sector, UINT count) {
  DRESULT r = ff_sd_read(pdrv, buffer, sector, count);
  for (int i = 1; r == RES_ERROR && i < kSdTries; i++) {
    sd_retries++;
    delay(2);
    r = ff_sd_read(pdrv, buffer, sector, count);
  }
  if (r == RES_ERROR) sd_failures++;
  return r;
}

static DRESULT sd_write_retry(uint8_t pdrv, const uint8_t* buffer, DWORD sector, UINT count) {
  DRESULT r = ff_sd_write(pdrv, buffer, sector, count);
  for (int i = 1; r == RES_ERROR && i < kSdTries; i++) {
    sd_retries++;
    delay(2);
    r = ff_sd_write(pdrv, buffer, sector, count);
  }
  if (r == RES_ERROR) sd_failures++;
  return r;
}

// SD.begin() takes the first free FatFs drive; ask for it first, then wrap it once mounted.
static bool sd_mount() {
  BYTE pdrv = 0xFF;
  if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) return false;
  if (!SD.begin(SD_CS, sd_spi, SD_HZ, "/sd")) return false;
  static const ff_diskio_impl_t impl = {&ff_sd_initialize, &ff_sd_status, &sd_read_retry,
                                        &sd_write_retry, &ff_sd_ioctl};
  ff_diskio_register(pdrv, &impl);
  return true;
}

static void cmd_sd() {
  if (!sd_ok) return say("# no SD card mounted");
  static const char* types[] = {"none", "MMC", "SD", "SDHC", "unknown"};
  uint8_t t = SD.cardType();
  say(String("# card ") + types[t > 4 ? 4 : t] + ", " + (uint32_t)(SD.cardSize() >> 20) +
      " MB, used " + (uint32_t)(SD.usedBytes() >> 20) + " of " +
      (uint32_t)(SD.totalBytes() >> 20) + " MB");
  say(String("# transfers retried ") + sd_retries.load() + ", failed after " + kSdTries + " tries " +
      sd_failures.load());
}

static void cmd_ls() {
  if (!sd_ok) return say("# no SD card mounted");
  std::vector<std::string> names = library.names();
  say(String("# ") + (int)names.size() + " files in the library");
  for (auto& n : names) {
    LibraryEntry e;
    if (!library.get(n, e)) continue;
    String l = String("#   ") + n.c_str() + "  " + e.size + " B, " + duration(e.meta.estimated_time);
    if (!e.printer_name.empty())
      l += String(", printer ") + e.printer_name.c_str() + (e.printer_size ? "" : " (stale)");
    say(l);
  }
}

static void cmd_info(const String& name) {
  LibraryEntry e;
  if (!library.get(name.c_str(), e)) return say("# no such file");
  say(String("# ") + name + ": " + e.size + " bytes");
  std::string m = e.meta.serialize();
  size_t pos = 0;
  while (pos < m.size()) {
    size_t nl = m.find('\n', pos);
    say("#   " + m.substr(pos, nl - pos));
    pos = nl + 1;
  }
  if (!e.printer_name.empty())
    say(String("#   printer: ") + e.printer_name.c_str() + ", " + e.printer_size + " bytes");
}

// A harmless job for the exit test, shaped like slicer output (header and footer comments)
// but with no heating and no motion: M117 messages and G4 dwells only.
static void cmd_mkjob(const String& arg) {
  if (!sd_ok) return say("# no SD card mounted");
  String name = arg;
  int secs = 60;
  int sp = arg.lastIndexOf(' ');
  if (sp > 0 && arg.substring(sp + 1).toInt() > 0) {
    secs = arg.substring(sp + 1).toInt();
    name = arg.substring(0, sp);
  }
  if (name.isEmpty()) return say("# usage: mkjob NAME [SECONDS]");
  std::string err;
  auto w = library.create(name.c_str(), err);
  if (!w) return say("# mkjob: " + err);
  auto put = [&](const std::string& s) { return w->write((const uint8_t*)s.data(), s.size()); };
  bool ok = put("; generated by Halyard mkjob, a harmless test job: M117 and G4 only\n");
  for (int i = 1; ok && i <= secs; i++)
    ok = put("M117 Halyard job " + std::to_string(i) + "/" + std::to_string(secs) + "\nG4 S1\n");
  ok = ok && put("M117 Halyard job done\n; estimated printing time (normal mode) = " +
                 std::to_string(secs) + "s\n");
  if (!ok) return say("# mkjob: write failed");  // the writer's destructor cleans up
  if (!w->finish(err)) return say("# mkjob: " + err);
  say(String("# mkjob: ") + name + ", " + secs + " s");
}

// Kilobytes of "M117 G1 X.. Y.. E.." lines: a random walk, so they compress about like
// sliced G-code, while printing them would only show messages.
static void cmd_mkbig(const String& arg) {
  if (!sd_ok) return say("# no SD card mounted");
  int sp = arg.lastIndexOf(' ');
  long kb = sp > 0 ? arg.substring(sp + 1).toInt() : 0;
  String name = sp > 0 ? arg.substring(0, sp) : String();
  if (name.isEmpty() || kb <= 0) return say("# usage: mkbig NAME KB");
  std::string err;
  auto w = library.create(name.c_str(), err);
  if (!w) return say("# mkbig: " + err);
  uint32_t seed = 12345;
  auto rnd = [&seed](int n) {
    seed = seed * 1103515245 + 12345;
    return (int)((seed >> 16) % n);
  };
  float x = 100, y = 100, e = 0;
  char line[64];
  std::string chunk;
  size_t total = 0, target = (size_t)kb * 1024;
  bool ok = true;
  uint32_t t0 = millis();
  while (ok && total < target) {
    x += (rnd(2001) - 1000) / 250.0f;
    y += (rnd(2001) - 1000) / 250.0f;
    e = rnd(100000) / 100000.0f;
    int n = snprintf(line, sizeof line, "M117 G1 X%.3f Y%.3f E%.5f\n", x, y, e);
    chunk.append(line, n);
    total += n;
    if (chunk.size() >= 4096) {
      ok = w->write((const uint8_t*)chunk.data(), chunk.size());
      chunk.clear();
    }
  }
  if (ok && !chunk.empty()) ok = w->write((const uint8_t*)chunk.data(), chunk.size());
  if (!ok) return say("# mkbig: write failed");
  if (!w->finish(err)) return say("# mkbig: " + err);
  say(String("# mkbig: ") + name + ", " + (uint32_t)total + " bytes in " + (millis() - t0) + " ms");
}

// ---- printer ----------------------------------------------------------------------------

static void cmd_status() {
  const Status& s = printer.status();
  const char* st = link_.state() == LinkState::Ready ? "ready"
                   : link_.state() == LinkState::Halted ? "HALTED" : "connecting";
  say(String("# link ") + st + " @" + link_.caps().baud + ", window " + (int)link_.window() +
      ", queued " + (int)link_.queued() + ", unanswered " + (int)link_.unanswered());
  if (!s.responding) say("# printer NOT ANSWERING (reconnect when it is safe)");
  if (s.temps_valid)
    say(String("# hotend ") + String(s.hotend, 1) + "/" + String(s.hotend_target, 0) +
        "  bed " + String(s.bed, 1) + "/" + String(s.bed_target, 0));
  String job = String("# job ") + job_name(s.job);
  if (!s.file.empty()) job += String(", file ") + s.file.c_str();
  if (s.sd_size)
    job += String(", ") + s.sd_pos + "/" + s.sd_size + " (" +
           String(100.0 * s.sd_pos / s.sd_size, 1) + "%)";
  say(job);
  if (delivery.busy()) {
    Delivery::Progress p = delivery.progress();
    String l = String("# hand-off ") + p.step + ": " + p.name.c_str();
    if (!p.printer_name.empty()) l += String(" -> ") + p.printer_name.c_str();
    if (p.total) l += String(", ") + String(100.0 * p.bytes / p.total, 1) + "%";
    say(l);
  }
}

static void cmd_caps() {
  const Caps& c = link_.caps();
  say(String("# firmware: ") + c.firmware.c_str());
  say(String("# baud ") + c.baud + ", ADVANCED_OK " + (c.advanced_ok ? "yes" : "no") +
      ", free buffer slots " + c.buffer_free);
  String on = "# on: ", off = "# off:";
  for (auto& kv : c.cap) (kv.second ? on : off) += String(" ") + kv.first.c_str();
  say(on);
  say(off);
}

static void cmd_stats() {
  const LinkStats& s = link_.stats();
  say(String("# sent ") + s.lines_sent + " lines / " + s.bytes_sent + " bytes, received " +
      s.lines_received + " lines");
  say(String("# resend requests ") + s.resend_requests + ", recoveries " + s.recoveries +
      ", lines resent " + s.lines_resent + ", retransmits " + s.retransmits);
  say(String("# oks inferred ") + s.inferred + ", lost " + s.lost + ", stray " + s.stray_oks +
      ", garbled lines " + s.garbled + ", connects " + s.connects);
}

static void cmd_pls() {
  printer.list_files([](bool ok, const std::vector<FileEntry>& files) {
    if (!ok) return say("# pls: FAILED");
    say(String("# ") + (int)files.size() + " files on the printer's card");
    for (auto& f : files) {
      String l = String("#   ") + f.name.c_str() + "  " + f.size;
      if (!f.long_name.empty() && f.long_name != f.name) l += String("  ") + f.long_name.c_str();
      say(l);
    }
  });
}

// Through the Service, so Mainsail shows a hand-off started here as well.
static void hand_off(const String& name, bool print) {
  std::string err;
  if (!service.hand_off(name.c_str(), print, err)) say("# refused: " + err);
}

// ---- settings (halyard.cfg) ---------------------------------------------------------------
//
// halyard.cfg on the card is the truth: written on a PC before the card goes in, or edited in
// Mainsail. NVS keeps a copy of the last settings read, so that without a card Halyard still
// joins the network and says what is wrong. The console edits the file too.

static Settings active;         // what Halyard runs with
static bool join_failed = false;
static uint32_t joining_since = 0;
static const uint32_t kJoinTimeoutMs = 30000;

static Settings nvs_settings() {
  Settings s;
  prefs.begin("halyard", true);
  s.ssid = prefs.getString("ssid").c_str();
  s.password = prefs.getString("pass").c_str();
  s.hostname = prefs.getString("host", "halyard").c_str();
  String t = prefs.getString("trusted", "");
  s.api_key = prefs.getString("apikey").c_str();
  s.update_source = prefs.getString("source").c_str();
  prefs.end();
  Access a;
  std::string err;
  if (t == "none") s.trusted = "";
  else if (t.length() && a.set_trusted(t.c_str(), err)) s.trusted = a.trusted();
  return s;
}

// Only what changed: NVS wears.
static void nvs_store(const Settings& s) {
  prefs.begin("halyard", false);
  auto put = [](const char* key, const std::string& v, const char* def) {
    if (v == def) {
      if (prefs.isKey(key)) prefs.remove(key);
    } else if (prefs.getString(key, def) != v.c_str()) {
      prefs.putString(key, v.c_str());
    }
  };
  put("ssid", s.ssid, "");
  put("pass", s.password, "");
  put("host", s.hostname, "halyard");
  put("trusted", s.trusted.empty() ? "none" : s.trusted, Access::kDefaultTrusted);
  put("apikey", s.api_key, "");
  put("source", s.update_source, "");
  prefs.end();
}

static std::string new_api_key() { return Access::new_api_key([] { return esp_random(); }); }

// Halyard's address, or what is wrong, on the printer's screen (the Service sends M117).
static void screen_update() {
  std::string t;
  if (!active.screen_messages) t = "";
  else if (!sd_ok) t = "Halyard: no SD card";
  else if (active.ssid.empty()) t = "Halyard: no WiFi set";
  else if (WiFi.status() == WL_CONNECTED) t = "Halyard " + std::string(WiFi.localIP().toString().c_str());
  else if (join_failed) t = "Halyard: can't join " + active.ssid;
  service.show_on_printer(t);
}

static void config_apply(const Settings& s, bool boot) {
  std::string err;
  access_.set_trusted(s.trusted, err);  // checked when the file was read
  access_.set_api_key(s.api_key);
  printer.set_binary(s.binary_transfer);
  updates::set_source(s.update_source);
  bool network = !s.same_network(active);
  active = s;
  service.set_hostname(s.hostname);
  screen_update();
  if (boot || !network) return;
  std::string why;
  if (service.can_restart(why)) {
    service.respond("// halyard.cfg: the WiFi settings changed, so Halyard restarts");
    service.reboot_soon();
  } else {
    service.respond("!! halyard.cfg: the WiFi settings apply at the next restart; " + why);
  }
}

// At boot, on FIRMWARE_RESTART (Mainsail's SAVE & RESTART), and after the console edits it.
static void config_load(bool boot) {
  Settings last = nvs_settings();
  std::vector<std::string> warnings;
  Settings s = last;
  if (sd_ok) {
    std::string text;
    if (!read_file(kConfigPath, text)) {
      text = settings_file(last);  // a new card: what Halyard ran with until now
      if (!write_file(kConfigPath, text)) warnings.push_back("cannot write halyard.cfg on the card");
    }
    Settings defaults;
    defaults.api_key = last.api_key;  // a bad key in the file keeps the old one
    s = parse_settings(text, defaults, warnings);
    if (s.api_key.empty()) {  // a new card, or the line emptied for a new key
      s.api_key = new_api_key();
      set_setting(text, "access", "api_key", s.api_key);
      write_file(kConfigPath, text);
    }
  } else if (s.api_key.empty()) {
    s.api_key = new_api_key();
  }
  nvs_store(s);
  config_apply(s, boot);
  for (auto& w : warnings) say("# " + w);
  if (warnings.empty()) say(sd_ok ? "# halyard.cfg read" : "# no card: settings from the last halyard.cfg");
  service.set_config_warnings(warnings);
}

// The console's wifi, trust and apikey: into the file, or NVS without a card.
using Values = std::vector<std::pair<const char*, std::string>>;
static void config_set(const char* section, const Values& values) {
  if (!sd_ok) {
    Settings s = active;
    for (auto& kv : values) {
      std::string id = std::string(section) + "." + kv.first;
      if (id == "wifi.ssid") s.ssid = kv.second;
      else if (id == "wifi.password") s.password = kv.second;
      else if (id == "access.trusted") s.trusted = kv.second == "none" ? "" : kv.second;
      else if (id == "access.api_key") s.api_key = kv.second;
    }
    nvs_store(s);
    config_apply(s, false);
    return;
  }
  std::string text;
  if (!read_file(kConfigPath, text)) text = settings_file(active);
  for (auto& kv : values) set_setting(text, section, kv.first, kv.second);
  if (!write_file(kConfigPath, text)) return say("# cannot write halyard.cfg");
  config_load(false);
}

// ---- network ----------------------------------------------------------------------------

static void wifi_connect() {
  if (active.ssid.empty()) return say("# no WiFi set: [wifi] in halyard.cfg, or wifi SSID PASSWORD");
  WiFi.setAutoReconnect(true);
  WiFi.begin(active.ssid.c_str(), active.password.c_str());
  joining_since = millis();
  say("# WiFi: joining " + active.ssid);
}

static void cmd_wifi(String arg) {
  if (arg.isEmpty()) {
    if (WiFi.status() == WL_CONNECTED)
      say("# WiFi: " + WiFi.SSID() + ", http://" + WiFi.localIP().toString() + "/ (" +
          active.hostname.c_str() + ".local), RSSI " + WiFi.RSSI() + " dBm");
    else
      say("# WiFi: not connected");
    return;
  }
  if (arg == "off") {
    config_set("wifi", {{"ssid", ""}, {"password", ""}});
    return say("# WiFi: forgotten");
  }
  String ssid, pass;
  if (arg[0] == '"') {
    int q = arg.indexOf('"', 1);
    if (q < 0) return say("# usage: wifi \"MY SSID\" PASSWORD");
    ssid = arg.substring(1, q);
    pass = arg.substring(q + 1);
  } else {
    int sp = arg.indexOf(' ');
    ssid = sp < 0 ? arg : arg.substring(0, sp);
    pass = sp < 0 ? String() : arg.substring(sp + 1);
  }
  pass.trim();
  config_set("wifi", {{"ssid", ssid.c_str()}, {"password", pass.c_str()}});
}

static void wifi_poll() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (!up && !join_failed && !active.ssid.empty() && millis() - joining_since > kJoinTimeoutMs) {
    join_failed = true;
    say("# WiFi: cannot join " + active.ssid);
    screen_update();
  }
  if (up == wifi_got_ip) return;
  wifi_got_ip = up;
  if (!up) {
    joining_since = millis();
    return say("# WiFi: disconnected");
  }
  join_failed = false;
  screen_update();
  updates::network_up();
  say("# WiFi: http://" + std::string(WiFi.localIP().toString().c_str()) + "/ (" +
      active.hostname + ".local)");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  if (!mdns_started && MDNS.begin(active.hostname.c_str())) {
    mdns_started = true;
    MDNS.addService("http", "tcp", 80);
    // Slicers look for OctoPrint (PrusaSlicer's "Browse", Cura's plugin).
    MDNS.addService("octoprint", "tcp", 80);
    MDNS.addServiceTxt("octoprint", "tcp", "path", "/");
    MDNS.addServiceTxt("octoprint", "tcp", "api", "0.1");
    MDNS.addServiceTxt("octoprint", "tcp", "version", "1.10.0");
  }
}

// ---- access -----------------------------------------------------------------------------

static void cmd_trust(String arg) {
  String sub = arg, rest;
  int sp = arg.indexOf(' ');
  if (sp > 0) {
    sub = arg.substring(0, sp);
    rest = arg.substring(sp + 1);
    rest.trim();
  }
  Access a;
  std::string err, list = active.trusted;
  if (sub == "add" && rest.length()) {
    list += (list.empty() ? "" : ", ") + std::string(rest.c_str());
  } else if (sub == "set" && rest.length()) {
    list = rest == "none" ? "" : rest.c_str();
  } else if (sub == "default") {
    list = Access::kDefaultTrusted;
  } else if (sub.length()) {
    return say("# usage: trust [add RANGE | set LIST | set none | default]");
  }
  if (sub.length()) {
    if (!a.set_trusted(list, err)) return say("# trust: " + err);
    config_set("access", {{"trusted", a.trusted().empty() ? "none" : a.trusted()}});
  }
  std::string t = access_.trusted();
  say("# trusted: " + (t.empty() ? std::string("none (every client needs the API key)") : t));
}

static void cmd_apikey(const String& arg) {
  if (arg == "new") config_set("access", {{"api_key", new_api_key()}});
  else if (arg.length()) return say("# usage: apikey [new]");
  say("# API key: " + access_.api_key());
}

// ---- firmware updates -----------------------------------------------------------------------

// The Arduino core keeps an OTA image on trial when this returns true (esp32-hal-misc.c):
// ota_poll() confirms it, or restarts into the previous image. extern "C": the weak default
// it overrides is C, and a C++ definition would not replace it (the image would be kept at
// once).
extern "C" bool verifyRollbackLater() { return true; }

static bool on_trial() {
  esp_ota_img_states_t st;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void cmd_version() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  say(std::string("# Halyard ") + HALYARD_VERSION + " built " + HALYARD_BUILT);
  say(std::string("# running from ") + run->label +
      (on_trial() ? " ON TRIAL (kept once it answers a request over WiFi)" : ""));
  static const char* reasons[] = {"unknown", "power-on", "external pin", "software restart",
                                  "panic", "interrupt watchdog", "task watchdog", "other watchdog",
                                  "deep sleep", "brownout", "SDIO", "USB", "JTAG", "efuse error",
                                  "power glitch", "CPU lockup"};  // esp_reset_reason_t's order
  int r = (int)esp_reset_reason();
  say(std::string("# last reset: ") +
      (r >= 0 && r < (int)(sizeof reasons / sizeof *reasons) ? reasons[r] : "?"));
  std::string st = updates::status();  // loop() holds core_lock, which this takes too
  std::string ui = sd_ok ? (webui.installed() ? webui.version() : "none") : "no card";
  say("# Mainsail: " + ui + " (this firmware ships with " HALYARD_MAINSAIL ")");
  if (!st.empty()) say("# " + st);
}

static void ota_poll() {
  static bool checked = false, trial = false;
  if (!checked) {
    checked = true;
    trial = on_trial();
    if (trial) say("# firmware update on trial: kept once it answers a request over WiFi");
  }
  if (!trial) return;
  if (web::served_any()) {
    trial = false;
    esp_ota_mark_app_valid_cancel_rollback();
    return say("# firmware update confirmed");
  }
  if (millis() > kTrialMs) {
    say("# firmware update never answered over WiFi: going back to the previous one");
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

static void handle(String line) {
  line.trim();
  if (line.isEmpty()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? String() : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") return help();
  if (cmd == "status") return cmd_status();
  if (cmd == "caps") return cmd_caps();
  if (cmd == "stats") return cmd_stats();
  if (cmd == "sd") return cmd_sd();
  if (cmd == "ls") return cmd_ls();
  if (cmd == "info") return cmd_info(arg);
  if (cmd == "mkjob") return cmd_mkjob(arg);
  if (cmd == "mkbig") return cmd_mkbig(arg);
  if (cmd == "wifi") return cmd_wifi(arg);
  if (cmd == "trust") return cmd_trust(arg);
  if (cmd == "config") return config_load(false);
  if (cmd == "apikey") return cmd_apikey(arg);
  if (cmd == "version") return cmd_version();
  if (cmd == "binary") {
    if (arg.length()) printer.set_binary(arg == "on");
    return say(String("# binary transfer ") + (printer.binary_enabled() ? "on" : "off"));
  }
  if (cmd == "rm") {
    if (delivery.busy() && delivery.progress().name == arg.c_str())
      return say("# rm refused: that file is being handed off");
    return say(library.remove(arg.c_str()) ? "# rm: ok" : "# rm: no such file");
  }
  if (cmd == "abort") {
    if (!delivery.busy()) return say("# no hand-off running");
    return delivery.abort();
  }
  if (cmd == "raw") {
    raw = arg == "on";
    return say(String("# raw ") + (raw ? "on" : "off"));
  }
  if (cmd == "reconnect") return link_.reconnect();
  if (link_.state() != LinkState::Ready) return say("# link not ready");
  if (cmd == "send") return hand_off(arg, false);
  if (cmd == "print") return hand_off(arg, true);
  if (cmd == "pls") return cmd_pls();
  if (cmd == "pprint") return printer.start_print(arg.c_str(), report("pprint"));
  if (cmd == "pause") return printer.pause(report("pause"));
  if (cmd == "resume") return printer.resume(report("resume"));
  if (cmd == "cancel") return printer.cancel(report("cancel"));
  if (cmd == "prm") {
    if (!printer.remove(arg.c_str(), report("prm"))) say("# prm refused");
    return;
  }
  if (cmd == "gc") {
    std::string c = arg.c_str();
    bool ok = printer.send_raw(c, [c](const Reply& r) {
      for (auto& l : r.lines) say("  " + l);
      say("# " + c + ": " + outcome_name(r.outcome) +
          (r.ok_line.size() > 2 ? " (" + r.ok_line + ")" : std::string()));
    });
    if (!ok) say("# gc refused (upload running, or not a sendable line)");
    return;
  }
  say("# unknown command; try help");
}

void setup() {
  core_lock = xSemaphoreCreateRecursiveMutex();
  // Buffer enough that a listing survives, and let flush() wait briefly: with a zero
  // timeout HWCDC leaves the last packet (often the newline) until the next write. With no
  // USB host it drops output at once; a host that isn't reading costs at most 10 ms a line.
  Serial.setTxTimeoutMs(10);
  Serial.setTxBufferSize(16384);
  Serial.begin(115200);
  Serial1.setRxBufferSize(4096);
  Serial1.begin(250000, SERIAL_8N1, UART_RX, UART_TX);

  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sd_ok = sd_mount() && library.begin();
  if (sd_ok) mkdir(kDbDir, 0755);
  service.begin();
  service.on_restart = [] { config_load(false); };
  webui.on_busy = [] { vTaskDelay(1); };  // lets core 0's idle task run (the task watchdog)
  updates::begin(service, webui, core_lock, HALYARD_MAINSAIL);
  // Start at the baud rate that worked last time: after a restart during a print, the
  // printer may be silent for seconds, and trying other rates would send it junk.
  prefs.begin("halyard", true);
  link_.set_known_baud(prefs.getUInt("baud", 0));
  prefs.end();
  service.on_console = [](const std::string& l) { say("# console: " + l); };

  printer.on_line = [](const std::string& l) {
    if (raw) say("< " + l);
    service.printer_line(l);
  };
  printer.on_event = [](const std::string& e) { say("# " + e); };
  delivery.on_event = [](const std::string& e) { service.delivery_event(e); };  // echoed below
  link_.on_note = [](const std::string& n) { say("# link: " + n); };
  printer.on_state = [](LinkState s) {
    if (s == LinkState::Ready) {
      prefs.begin("halyard", false);
      if (prefs.getUInt("baud", 0) != link_.caps().baud) prefs.putUInt("baud", link_.caps().baud);
      prefs.end();
    }
    say(std::string("# link ") + (s == LinkState::Ready ? "ready: " + link_.caps().firmware
                                  : s == LinkState::Halted ? "HALTED (reset the printer)"
                                                           : "connecting"));
  };
  help();
  cmd_version();
  say(sd_ok ? "# SD card mounted, library at /sd/gcodes"
            : "# NO SD CARD it can read (FAT32, 32 GB or smaller): library unavailable");
  if (sd_ok) say(webui.installed() ? "# Mainsail " + webui.version() + " on the card"
                                   : std::string("# no Mainsail on the card: Halyard fetches ") +
                                         HALYARD_MAINSAIL + " once WiFi is up");
  // The station interface first: it brings up the TCP/IP stack the web server needs, with
  // or without a network configured.
  config_load(true);  // after the card, before the network; prints its warnings
  WiFi.setHostname(active.hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds latency to every request
  say(web::begin(service, access_, webui, core_lock) ? "# web server on port 80" : "# web server FAILED to start");
  wifi_connect();
}

void loop() {
  xSemaphoreTakeRecursive(core_lock, portMAX_DELAY);
  uint32_t now = millis();
  printer.poll(now);
  delivery.poll(now);
  service.poll(now);
  bool pending = service.pending();
  static uint32_t last_flush = 0;
  if (millis() - last_flush >= 20) {  // push out a trailing partial USB packet (see setup)
    last_flush = millis();
    Serial.flush();
  }
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (input.length()) {
        Serial.println();
        handle(input);
        input = "";
      }
    } else if (c == 0x08 || c == 0x7f) {
      if (input.length()) {
        input.remove(input.length() - 1);
        Serial.print("\b \b");
      }
    } else if (input.length() < 120) {
      input += c;
      Serial.print(c);
    }
  }
  xSemaphoreGiveRecursive(core_lock);
  if (pending) web::flush();
  wifi_poll();
  ota_poll();
  // Let the web server's task take the lock; the UART buffers far more than a millisecond.
  vTaskDelay(1);
}
