// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.h"

#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "gcode_line.h"
#include "settings.h"

namespace halyard {

namespace {

// Mainsail warns below v0.8.0-306 (mainsail-api-log.md, finding 10). Halyard implements that
// API level for the methods it offers.
const char* kMoonrakerVersion = "v0.9.3-0";

const int kBadRequest = 400, kNotFound = 404, kConflict = 409, kUnavailable = 503;
const int kMethodNotFound = -32601, kInvalidRequest = -32600, kParseError = -32700;

const char* const kConfigName = "halyard.cfg";
const char* const kMarlinName = "marlin.cfg";  // M503's report, made on download
const size_t kMaxConfig = 16 * 1024;

const uint32_t kStatusPushMs = 250, kTempSampleMs = 1000, kDbSaveMs = 2000;
const uint32_t kProcStatMs = 5000;  // notify_proc_stat_update; Moonraker's is every second
const size_t kHistoryMax = 50, kGcodeStoreMax = 100, kOutboxMax = 300;
const double kMinExtrudeTemp = 170;  // Marlin's EXTRUDE_MINTEMP default
// The same limits the hand-off applies to preheat temperatures (delivery.cpp).
const double kMaxHotend = 300, kMaxBed = 130;

std::string to_json(JsonVariantConst v) {
  std::string s;
  serializeJson(v, s);
  return s;
}

std::string str(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.isNull()) return "";
  return to_json(v);
}

double num(JsonVariantConst v, double def) {
  if (v.is<double>()) return v.as<double>();
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    char* end;
    double d = strtod(s, &end);
    return end != s ? d : def;
  }
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  return def;
}

bool truthy(JsonVariantConst v) {
  if (v.is<bool>()) return v.as<bool>();
  std::string s = str(v);
  return s == "true" || s == "1" || s == "True";
}

double round2(double d) { return std::round(d * 100) / 100; }

std::string upper(std::string s) {
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
               isxdigit((unsigned char)s[i + 2])) {
      out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// "a=1&b=x%20y&flag" into an object of strings (a bare key gets an empty string).
void parse_query(const std::string& q, JsonObject out) {
  size_t pos = 0;
  while (pos < q.size()) {
    size_t e = q.find('&', pos);
    if (e == std::string::npos) e = q.size();
    std::string kv = q.substr(pos, e - pos);
    pos = e + 1;
    if (kv.empty()) continue;
    size_t eq = kv.find('=');
    std::string k = url_decode(kv.substr(0, eq));
    std::string v = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
    out[k] = v;
  }
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    size_t e = s.find(sep, pos);
    out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
    if (e == std::string::npos) return out;
    pos = e + 1;
  }
}

// "gcodes/name.gcode" -> "name.gcode"; false for another root.
bool strip_gcodes_root(std::string path, std::string& name) {
  while (!path.empty() && path[0] == '/') path.erase(0, 1);
  if (path.compare(0, 7, "gcodes/") != 0) return false;
  name = path.substr(7);
  return true;
}

// A G-code word like G28, M104 or T0, as opposed to a Klipper-style extended command.
bool is_gcode_word(const std::string& w) {
  if (w.size() < 2 || (w[0] != 'G' && w[0] != 'M' && w[0] != 'T')) return false;
  for (size_t i = 1; i < w.size(); i++)
    if (!isdigit((unsigned char)w[i]) && w[i] != '.') return false;
  return true;
}

// "KEY=value" among the words of a Klipper command.
bool klipper_arg(const std::vector<std::string>& words, const std::string& key, std::string& out) {
  for (auto& w : words) {
    size_t eq = w.find('=');
    if (eq != std::string::npos && upper(w.substr(0, eq)) == key) {
      out = w.substr(eq + 1);
      return true;
    }
  }
  return false;
}

std::vector<std::string> words_of(const std::string& line) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < line.size()) {
    size_t a = line.find_first_not_of(" \t", pos);
    if (a == std::string::npos) break;
    size_t b = line.find_first_of(" \t", a);
    out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    pos = b == std::string::npos ? line.size() : b;
  }
  return out;
}

// Printer text is ASCII. Anything else is line noise: the ROM boot log the GEEK leaks onto the
// printer's RX at every reset, echoed back in "Unknown command", or a stray binary SYNC. Raw,
// it is invalid UTF-8 and control bytes in a websocket text frame, and the browser drops the
// connection over it.
bool is_ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c < 0x20 || c > 0x7e) return false;
  return true;
}

std::string ascii(std::string s) {
  for (auto& c : s)
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
  return s;
}

// Worth showing in Mainsail's console: not the printer's periodic reports.
bool console_worthy(const std::string& raw) {
  std::string t = trim(raw);
  if (t.empty()) return false;
  if (starts_with(t, "T:") || starts_with(t, "ok")) return false;  // temperature reports
  if (starts_with(t, "busy:") || starts_with(t, "echo:busy")) return false;
  if (starts_with(t, "SD printing byte") || t == "Not SD printing") return false;
  return true;
}

// Marlin's "echo:" prefix means nothing to a person; "Error:" is marked as Klipper does.
std::string console_text(const std::string& raw) {
  std::string t = trim(raw);
  if (starts_with(t, "echo:")) return trim(t.substr(5));
  if (starts_with(t, "Error:")) return "!! " + t;
  if (starts_with(t, "//action:")) return "// action:" + t.substr(9);
  return t;
}

// Commands Mainsail's console offers to complete, with the Klipper ones Halyard translates.
const char* const kCommands[][2] = {
    {"G28", "Home all axes, or those named (G28 X Y)"},
    {"G0", "Move (G0 X10 Y10 F3000)"},
    {"G1", "Move (G1 X10 Y10 F3000)"},
    {"G90", "Absolute positioning"},
    {"G91", "Relative positioning"},
    {"M104", "Set the hotend temperature (M104 S200)"},
    {"M109", "Set the hotend temperature and wait"},
    {"M140", "Set the bed temperature (M140 S60)"},
    {"M190", "Set the bed temperature and wait"},
    {"M105", "Report temperatures"},
    {"M106", "Fan on (M106 S255)"},
    {"M107", "Fan off"},
    {"M114", "Report the position"},
    {"M115", "Report the firmware and its capabilities"},
    {"M117", "Show a message on the printer's screen"},
    {"M119", "Report the endstops"},
    {"M220", "Speed factor (M220 S100)"},
    {"M221", "Flow factor (M221 S100)"},
    {"M84", "Disable the steppers"},
    {"M500", "Save settings to EEPROM"},
    {"M503", "Report settings"},
    {"SET_HEATER_TEMPERATURE", "Klipper style: HEATER=extruder|heater_bed TARGET=<temp>"},
    {"TURN_OFF_HEATERS", "Switch both heaters off"},
    {"PAUSE", "Pause the print"},
    {"RESUME", "Resume the print"},
    {"CANCEL_PRINT", "Cancel the print"},
    {"SDCARD_RESET_FILE", "Clear the finished job from the status panel"},
    {"BED_PID_TUNE", "PID autotune of the bed (M303), TARGET=60 by default"},
    {"EXTRUDER_PID_TUNE", "PID autotune of the hotend (M303), TARGET=210 by default"},
    {"PID_CALIBRATE", "Klipper style: HEATER=extruder|heater_bed TARGET=<temp> (M303)"},
    {"CALIBRATE_Z_OFFSET", "Home, probe the bed's centre, then find the nozzle's height by hand"},
    {"TESTZ", "During CALIBRATE_Z_OFFSET: move the nozzle (Z=-0.1, or + - ++ -- to bisect)"},
    {"ACCEPT", "During CALIBRATE_Z_OFFSET: set the probe's Z offset (M851) from here"},
    {"ABORT", "During CALIBRATE_Z_OFFSET: leave the offset as it was"},
    {"SAVE_CONFIG", "Keep calibration results in the printer's EEPROM (M500)"},
    {"SET_GCODE_OFFSET", "Babystep Z (M290): Z_ADJUST=<mm>, or Z=<mm> for the total"},
    {"Z_OFFSET_APPLY_PROBE", "Add the Z babysteps to the probe's Z offset (M851); SAVE_CONFIG keeps it"},
    {"SET_PRESSURE_ADVANCE", "Marlin's Linear Advance (M900 K): ADVANCE=<k>"},
    {"SET_VELOCITY_LIMIT", "Marlin's maximum feedrate (M203 X Y) and acceleration (M204 P T): VELOCITY=<mm/s> ACCEL=<mm/s2>"},
    {"SAVE_GCODE_STATE", "Before Mainsail's moves: Marlin has nothing to save"},
    {"RESTORE_GCODE_STATE", "After Mainsail's moves: absolute coordinates and extrusion (G90, M82)"},
    {"FIRMWARE_RESTART", "Read halyard.cfg again and reconnect to the printer"},
    {"RESTART", "Read halyard.cfg again and reconnect to the printer"},
    {"HELP", "List these commands"},
};

// The calibration macros in Mainsail's macro panel. Mainsail reads a macro's parameters from
// its gcode text ({params.X|default(v)}), so the text is written for that.
struct Macro {
  const char* name;
  const char* gcode;
  bool probe;  // only with a Z probe (M115 Cap:Z_PROBE)
};
const Macro kMacros[] = {
    {"BED_PID_TUNE", "PID_CALIBRATE HEATER=heater_bed TARGET={params.TARGET|default(60)|int}", false},
    {"EXTRUDER_PID_TUNE", "PID_CALIBRATE HEATER=extruder TARGET={params.TARGET|default(210)|int}",
     false},
    {"CALIBRATE_Z_OFFSET", "G28\nG30 at the bed's centre\nPROBE_CALIBRATE (TESTZ, ACCEPT)", true},
};
const double kBedPidTarget = 60, kHotendPidTarget = 210;
const double kBisectMax = 0.2;  // TESTZ + and -, as Klipper's manual_probe

std::string fmt3(double v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

// The number after `key` in a printer line, e.g. " Z" in "  M851 X-33.00 Y-15.00 Z-0.90 ;".
bool number_after(const std::string& line, const char* key, double& v) {
  size_t p = line.find(key);
  if (p == std::string::npos) return false;
  const char* a = line.c_str() + p + strlen(key);
  char* end;
  v = strtod(a, &end);
  return end != a;
}

// A number in a settings report without parameters, e.g. " Y" in M203's
// "  M203 X500.00 Y500.00 Z5.00 E25.00". False if the printer did not report it.
bool report_value(const std::vector<std::string>& lines, const char* code, const char* key,
                  double& v) {
  for (auto& l : lines) {
    size_t at = l.find(code);
    if (at != std::string::npos && number_after(l.substr(at), key, v)) return true;
  }
  return false;
}

// Linear Advance's K from M900's report: "echo:Advance K=0.05", with ADVANCE_K_EXTRA
// "echo:Advance S0 K0.05(S1 K0.00)", with several extruders "echo:Advance K 0:0.05 1:0.02"
// (the first). False if the printer has no LIN_ADVANCE ("Unknown command").
bool parse_advance(const std::vector<std::string>& lines, double& k) {
  for (auto& l : lines) {
    size_t a = l.find("Advance");
    size_t kp = a == std::string::npos ? a : l.find('K', a);
    if (kp == std::string::npos) continue;
    const char* p = l.c_str() + kp + 1;
    while (*p == '=' || *p == ' ') p++;
    if (isdigit((unsigned char)p[0]) && p[1] == ':') p += 2;
    char* end;
    double v = strtod(p, &end);
    if (end == p) continue;
    k = v;
    return true;
  }
  return false;
}

// M114: "X:10.00 Y:20.00 Z:5.00 E:0.00 Count X:800 Y:1600 Z:2000".
bool parse_position(const std::string& line, double pos[4]) {
  std::string t = trim(line);
  if (!starts_with(t, "X:")) return false;
  double v[4];
  const char* keys[] = {"X:", " Y:", " Z:", " E:"};
  for (int i = 0; i < 4; i++)
    if (!number_after(t, keys[i], v[i])) return false;
  for (int i = 0; i < 4; i++) pos[i] = v[i];
  return true;
}

// The axes a G28 homes: those it names ("G28 X Y", "G28XY"), or all of them.
std::string homed_by(const std::string& g28) {
  std::string axes;
  for (size_t i = 3; i < g28.size(); i++) {
    char c = (char)tolower((unsigned char)g28[i]);
    if (c == ';') break;
    if ((c == 'x' || c == 'y' || c == 'z') && axes.find(c) == std::string::npos) axes += c;
  }
  return axes.empty() ? "xyz" : axes;
}

std::string merge_axes(const std::string& a, const std::string& b) {
  std::string out;
  for (char c : std::string("xyz"))
    if (a.find(c) != std::string::npos || b.find(c) != std::string::npos) out += c;
  return out;
}

// What a move needs homed (Klipper refuses the rest): the axes a G0/G1 names, and X and Y
// for an arc (G2/G3) or a spline (G5), which move in the plane without naming it.
std::string axes_moved(const std::string& line) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos || toupper((unsigned char)line[i]) != 'G') return "";
  char* end;
  long n = strtol(line.c_str() + i + 1, &end, 10);
  if (end == line.c_str() + i + 1 || *end == '.' || n < 0 || n > 5 || n == 4) return "";
  std::string axes = n == 0 || n == 1 ? "" : "xy";
  for (const char* c = end; *c && *c != ';' && *c != '*'; c++) {
    char a = (char)tolower((unsigned char)*c);
    if ((a == 'x' || a == 'y' || a == 'z') && axes.find(a) == std::string::npos) axes += a;
  }
  return merge_axes(axes, "");
}

// Whether the first word of a line is this G- or M-code, as Marlin reads it: "G28X" is
// G28 X, "G280" is not G28.
bool is_code(const std::string& word, const char* code) {
  size_t n = strlen(code);
  return word.compare(0, n, code) == 0 &&
         (word.size() == n || !(isdigit((unsigned char)word[n]) || word[n] == '.'));
}

// The G- or M-code a line starts with, as Marlin reads it: "m92x80" is M92.
std::string code_of(const std::string& line) {
  size_t start = line.find_first_not_of(' ');
  std::string t = start == std::string::npos ? "" : upper(line.substr(start));
  if (t.empty() || !isalpha((unsigned char)t[0])) return "";
  size_t n = 1;
  while (n < t.size() && isdigit((unsigned char)t[n])) n++;
  return n > 1 ? t.substr(0, n) : "";
}

// Commands that move the steppers, or keep them on while they wait for a heater, so
// Marlin's idle timeout starts again as they end.
bool keeps_steppers_on(const std::string& code) {
  static const char* moves[] = {"G0", "G1", "G2", "G3", "G5", "G28", "G29", "G30", "G34",
                                "M109", "M190"};
  for (auto m : moves)
    if (is_code(code, m)) return true;
  return false;
}

// M18/M84's arguments, and S: the idle timeout it sets in seconds (then it switches
// nothing off).
std::string m84_args(const std::string& gcode) {
  size_t c = gcode.find(';');
  return upper(gcode.substr(3, c == std::string::npos ? std::string::npos : c - 3));
}

bool m84_timeout(const std::string& args, double& sec) {
  size_t s = args.find('S');
  if (s == std::string::npos || s + 1 >= args.size() ||
      !(isdigit((unsigned char)args[s + 1]) || args[s + 1] == '.'))
    return false;
  sec = strtod(args.c_str() + s + 1, nullptr);
  return true;
}

// The axes an M18/M84 switches off: those it names, or all of them. None with S, or with
// E alone.
std::string switched_off(const std::string& gcode) {
  std::string args = m84_args(gcode), off;
  double sec;
  if (m84_timeout(args, sec)) return "";
  for (char c : args)
    if (c == 'X' || c == 'Y' || c == 'Z') off += (char)tolower((unsigned char)c);
  if (off.empty() && args.find('E') == std::string::npos) off = "xyz";
  return off;
}

std::string without_axes(const std::string& axes, const std::string& off) {
  std::string out;
  for (char c : axes)
    if (off.find(c) == std::string::npos) out += c;
  return out;
}

const uint32_t kStepperTimeoutMs = 120000;  // Marlin's DEFAULT_STEPPER_TIMEOUT_SEC
const uint32_t kPositionPollMs = 5000;  // M114 while idle, and after each console script
const uint32_t kFactorsPollMs = 10000;  // M220 and M221, printing too

const char* state_name(int s) {
  static const char* names[] = {"standby", "printing", "paused", "complete", "cancelled", "error"};
  return names[s];
}

}  // namespace

const char* const Service::kNoCard =
    "Halyard has no SD card it can read: uploads and prints from its library are off. Put a "
    "FAT32 microSD card (32 GB or smaller; larger ones come as exFAT) in the GEEK and restart it.";

// ---- construction ----------------------------------------------------------------------

Service::Service(Link& link, Printer& printer, Delivery& delivery, Library& library,
                 Platform platform)
    : link_(link), printer_(printer), delivery_(delivery), lib_(library),
      platform_(std::move(platform)) {
  methods_ = {
      {"server.connection.identify", &Service::m_identify},
      {"server.info", &Service::m_server_info},
      {"server.config", &Service::m_server_config},
      {"printer.info", &Service::m_printer_info},
      {"machine.system_info", &Service::m_system_info},
      {"machine.proc_stats", &Service::m_proc_stats},
      {"machine.update.status", &Service::m_update_status},
      {"machine.update.refresh", &Service::m_update_refresh},
      {"machine.update.client", &Service::m_update_client},
      {"machine.update.upgrade", &Service::m_update_client},
      {"machine.update.full", &Service::m_update_full},
      {"machine.reboot", &Service::m_machine_reboot},
      {"server.webcams.list", &Service::m_webcams},
      {"server.announcements.list", &Service::m_announcements},
      {"printer.objects.list", &Service::m_objects_list},
      {"printer.objects.query", &Service::m_objects_query},
      {"printer.objects.subscribe", &Service::m_objects_subscribe},
      {"printer.gcode.script", &Service::m_gcode_script},
      {"printer.gcode.help", &Service::m_gcode_help},
      {"server.gcode_store", &Service::m_gcode_store},
      {"server.temperature_store", &Service::m_temperature_store},
      {"printer.print.start", &Service::m_print_start},
      {"printer.print.pause", &Service::m_print_pause},
      {"printer.print.resume", &Service::m_print_resume},
      {"printer.print.cancel", &Service::m_print_cancel},
      {"printer.emergency_stop", &Service::m_emergency_stop},
      {"printer.restart", &Service::m_restart},
      {"printer.firmware_restart", &Service::m_restart},
      {"printer.query_endstops.status", &Service::m_query_endstops},
      {"server.files.list", &Service::m_files_list},
      {"server.files.get_directory", &Service::m_files_directory},
      {"server.files.metadata", &Service::m_files_metadata},
      {"server.files.delete_file", &Service::m_files_delete},
      {"server.files.move", &Service::m_files_move},
      {"server.files.zip", &Service::m_files_zip},
      {"server.files.post_directory", &Service::m_files_no_folders},
      {"server.files.delete_directory", &Service::m_files_no_folders},
      {"server.files.copy", &Service::m_files_no_folders},
      {"server.database.list", &Service::m_db_list},
      {"server.database.get_item", &Service::m_db_get},
      {"server.database.post_item", &Service::m_db_post},
      {"server.database.delete_item", &Service::m_db_delete},
      {"server.history.list", &Service::m_history_list},
      {"server.history.totals", &Service::m_history_totals},
      {"printer.card.list", &Service::m_card_list},
      {"printer.card.print", &Service::m_card_print},
      {"printer.card.delete", &Service::m_card_delete},
      {"printer.card.copy", &Service::m_card_copy},
  };
  for (auto& t : temps_) t.assign(kTempSamples, 0);
  for (auto& p : powers_) p.assign(kTempSamples, 0);
  db_.to<JsonObject>();
}

Service::~Service() = default;

void Service::begin() {
  db_load();
  // Up to 0.5.0 a navi.json beside the database put the update page in Mainsail's sidebar.
  // The Update Manager does its work now, and the page is a fallback at /halyard/.
  const std::string& db = platform_.db_path;
  size_t slash = db.rfind('/');
  if (slash != std::string::npos) ::remove((db.substr(0, slash) + "/navi.json").c_str());
}

double Service::wall() const {
  double t = platform_.wall_time ? platform_.wall_time() : 0;
  return t > 0 ? t : uptime();
}

const char* Service::klippy_state() const {
  switch (link_.state()) {
    case LinkState::Ready: return "ready";
    case LinkState::Halted: return "shutdown";
    default: return "startup";
  }
}

static std::string klippy_message(const std::string& state) {
  if (state == "ready") return "Printer is ready";
  if (state == "shutdown")
    return "The printer halted (M112 or a firmware fault). Reset it, then press FIRMWARE RESTART.";
  return "Halyard is connecting to the printer on its serial port.";
}

// ---- websocket -------------------------------------------------------------------------

void Service::ws_open(int conn) { conns_[conn] = Conn(); }

void Service::ws_close(int conn) {
  conns_.erase(conn);
  for (auto it = outbox_.begin(); it != outbox_.end();)
    it = it->first == conn ? outbox_.erase(it) : it + 1;
}

bool Service::take(int& conn, std::string& text) {
  if (outbox_.empty()) return false;
  conn = outbox_.front().first;
  text = std::move(outbox_.front().second);
  outbox_.pop_front();
  return true;
}

static std::string error_json(const std::string& id, int code, const std::string& msg) {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  JsonObject e = d["error"].to<JsonObject>();
  e["code"] = code;
  e["message"] = msg;
  std::string s = to_json(d);
  s.pop_back();
  return s + ",\"id\":" + (id.empty() ? "null" : id) + "}";
}

static std::string result_json(const std::string& id, const JsonDocument& result) {
  return "{\"jsonrpc\":\"2.0\",\"result\":" + to_json(result) + ",\"id\":" + id + "}";
}

std::string Service::ws_message(int conn, const std::string& text) {
  if (!conns_.count(conn)) ws_open(conn);
  JsonDocument doc;
  if (deserializeJson(doc, text, DeserializationOption::NestingLimit(32)))
    return error_json("", kParseError, "Parse error");
  if (!doc.is<JsonArray>()) return rpc(conn, doc.as<JsonVariantConst>());
  // A batch (Mainsail sends server.files.metadata this way): an array of the replies.
  std::string out;
  for (JsonVariantConst req : doc.as<JsonArrayConst>()) {
    std::string r = rpc(conn, req);
    if (r.empty()) continue;
    out += out.empty() ? "[" : ",";
    out += r;
  }
  return out.empty() ? out : out + "]";
}

std::string Service::rpc(int conn, JsonVariantConst req) {
  if (!req.is<JsonObjectConst>() || !req["method"].is<const char*>())
    return error_json("", kInvalidRequest, "Invalid Request");
  Call c;
  c.conn = conn;
  if (!req["id"].isNull()) c.id = to_json(req["id"]);
  JsonDocument result;
  std::string err;
  int code = dispatch(c, req["method"].as<const char*>(), req["params"], result, err);
  if (code == kDeferred || c.id.empty()) return "";
  return code ? error_json(c.id, code, err) : result_json(c.id, result);
}

void Service::reply(const Call& c, int code, const JsonDocument& result, const std::string& err) {
  if (c.id.empty() || c.conn < 0 || !conns_.count(c.conn)) return;
  outbox_.emplace_back(c.conn, code ? error_json(c.id, code, err) : result_json(c.id, result));
}

int Service::dispatch(const Call& c, const std::string& method, JsonVariantConst params,
                      JsonDocument& result, std::string& err) {
  auto it = methods_.find(method);
  if (it == methods_.end()) {
    err = "Method not found";
    return kMethodNotFound;
  }
  return (this->*(it->second))(c, params, result, err);
}

void Service::notify_all(const std::string& json) {
  for (auto& kv : conns_) outbox_.emplace_back(kv.first, json);
  while (outbox_.size() > kOutboxMax) outbox_.pop_front();
}

void Service::notify(const std::string& method, JsonVariantConst params, int only_conn) {
  std::string s = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\"";
  if (!params.isNull()) s += ",\"params\":" + to_json(params);
  s += "}";
  if (only_conn >= 0) {
    outbox_.emplace_back(only_conn, s);
    while (outbox_.size() > kOutboxMax) outbox_.pop_front();
  } else {
    notify_all(s);
  }
}

// ---- HTTP ------------------------------------------------------------------------------

bool Service::http(const std::string& method, const std::string& path, const std::string& query,
                   const std::string& body, HttpResponse& out) {
  out = HttpResponse();
  // OctoPrint, for slicers (api-contracts.md section 1).
  if (path == "/api/version") {
    out.body = "{\"api\":\"0.1\",\"server\":\"1.10.0\",\"text\":\"OctoPrint (Halyard " +
               platform_.version + ")\"}";
    return true;
  }
  if (path == "/api/server") {
    out.body = "{\"server\":\"1.10.0\",\"safemode\":null}";
    return true;
  }
  if (path == "/api/login") {  // Cura's OctoPrint plugin
    out.body = "{\"_is_external_client\":false,\"name\":\"_api\",\"active\":true,\"user\":true,"
               "\"admin\":true,\"apikey\":null,\"permissions\":[],\"groups\":[\"admins\",\"users\"]}";
    return true;
  }

  // Moonraker: every method is also a route, /printer/objects/query for printer.objects.query.
  std::string rpc_method;
  JsonDocument params;
  params.to<JsonObject>();
  if (method == "DELETE" && starts_with(path, "/server/files/") &&
      !starts_with(path, "/server/files/directory")) {
    rpc_method = "server.files.delete_file";
    params["path"] = url_decode(path.substr(14));
  } else if (starts_with(path, "/server/") || starts_with(path, "/printer/") ||
             starts_with(path, "/machine/") || starts_with(path, "/access/")) {
    rpc_method = path.substr(1);
    std::replace(rpc_method.begin(), rpc_method.end(), '/', '.');
    if (rpc_method == "server.files.directory")
      rpc_method = method == "POST" ? "server.files.post_directory"
                   : method == "DELETE" ? "server.files.delete_directory"
                                        : "server.files.get_directory";
    else if (rpc_method == "server.database.item")
      rpc_method = method == "POST" ? "server.database.post_item"
                   : method == "DELETE" ? "server.database.delete_item"
                                        : "server.database.get_item";
    parse_query(query, params.as<JsonObject>());
    if (!body.empty()) {
      JsonDocument b;
      if (!deserializeJson(b, body, DeserializationOption::NestingLimit(32)) && b.is<JsonObject>())
        for (JsonPairConst kv : b.as<JsonObjectConst>()) params[kv.key()] = kv.value();
    }
    // Query arguments name objects: ?extruder=temperature,target&webhooks
    if ((rpc_method == "printer.objects.query" || rpc_method == "printer.objects.subscribe") &&
        !params["objects"].is<JsonObject>()) {
      JsonDocument objs;
      JsonObject o = objs.to<JsonObject>();
      for (JsonPairConst kv : params.as<JsonObjectConst>()) {
        std::string k = kv.key().c_str();
        if (k == "connection_id") continue;
        std::string v = str(kv.value());
        if (v.empty()) {
          o[k] = nullptr;
        } else {
          JsonArray a = o[k].to<JsonArray>();
          for (auto& f : split(v, ',')) a.add(f);
        }
      }
      params["objects"] = objs;
    }
  } else {
    return false;
  }

  Call c;  // HTTP: answered now, never deferred
  JsonDocument result;
  std::string err;
  int code = dispatch(c, rpc_method, params.as<JsonVariantConst>(), result, err);
  if (code == 0 || code == kDeferred) {
    out.body = "{\"result\":" + to_json(result) + "}";
    return true;
  }
  out.status = code >= 400 && code < 600 ? code : code == kMethodNotFound ? 404 : 400;
  JsonDocument e;
  e["error"]["code"] = out.status;
  e["error"]["message"] = err;
  out.body = to_json(e);
  return true;
}

std::string Service::download_path(const std::string& path) const {
  std::string rel, decoded = url_decode(path);
  if (!strip_gcodes_root(decoded, rel)) return "";
  bool ok = Library::valid_name(rel);
  if (!ok && starts_with(rel, ".thumbs/")) {
    std::string t = rel.substr(8);
    ok = !t.empty() && t[0] != '.' && t.find('/') == std::string::npos &&
         t.find('\\') == std::string::npos;
  }
  if (!ok) return "";
  std::string full = lib_.root() + "/" + rel;
  struct stat st;
  if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return "";
  return full;
}

std::string Service::content_disposition(const std::string& path) {
  size_t slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  // A plain ASCII name for old browsers, and the real one in RFC 5987 form.
  std::string ascii, utf8;
  static const char hex[] = "0123456789ABCDEF";
  for (unsigned char c : name) {
    ascii += c >= 0x20 && c < 0x7f && c != '"' && c != '\\' ? (char)c : '_';
    if (isalnum(c) || strchr("!#$&+-.^_`|~", c)) {
      utf8 += (char)c;
    } else {
      utf8 += '%';
      utf8 += hex[c >> 4];
      utf8 += hex[c & 15];
    }
  }
  return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + utf8;
}

// ---- uploads ---------------------------------------------------------------------------

// Why a file can't be saved in the config root.
static std::string config_refusal(const std::string& filename) {
  if (filename == kMarlinName)
    return "marlin.cfg is read-only: send a setting's command on the console, then SAVE CONFIG (M500)";
  return "only halyard.cfg can be saved in the config root";
}

std::unique_ptr<Service::Upload> Service::upload(bool octoprint, const std::string& content_type,
                                                 HttpResponse& refused) {
  std::string boundary;
  if (!MultipartParser::boundary_from(content_type, boundary)) {
    refused.status = 400;
    refused.body = octoprint ? "{\"error\":\"expected multipart/form-data\"}"
                             : "{\"error\":{\"code\":400,\"message\":\"expected multipart/form-data\"}}";
    return nullptr;
  }
  return std::unique_ptr<Upload>(new Upload(*this, octoprint, boundary));
}

Service::Upload::Upload(Service& s, bool octoprint, const std::string& boundary)
    : s_(s), octoprint_(octoprint), parser_(boundary) {
  parser_.on_part = [this](const std::string& n, const std::string& f) { return part(n, f); };
  parser_.on_data = [this](const uint8_t* d, size_t len) { return data(d, len); };
  parser_.on_part_end = [this] { return part_end(); };
}

Service::Upload::~Upload() {
  writer_.reset();  // an unfinished file is deleted
  if (!name_.empty()) s_.uploading_.erase(name_);
}

bool Service::Upload::fail(int status, const std::string& msg) {
  if (!status_) {
    status_ = status;
    error_ = msg;
  }
  return false;
}

bool Service::Upload::part(const std::string& name, const std::string& filename) {
  field_ = name;
  value_.clear();
  if (name != "file") return true;
  if (s_.card_missing()) return fail(kUnavailable, kNoCard);
  if (writer_ || stored_) return fail(400, "one file per upload");
  // Mainsail's editor sends the file before its root, so halyard.cfg waits in memory.
  if (!s_.platform_.config_path.empty() && (root_ == "config" || filename == kConfigName)) {
    if (filename != kConfigName) return fail(400, config_refusal(filename));
    cfg_ = in_file_ = true;
    name_ = filename;
    return true;
  }
  if (root_ != "gcodes") return fail(400, "uploads go to the gcodes root");
  if (!path_.empty() && path_ != "/") return fail(400, "Halyard's library has no folders");
  if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos)
    return fail(400, "Halyard's library has no folders: " + filename);
  if (!Library::valid_name(filename)) return fail(400, "not a valid file name: " + filename);
  if (s_.uploading_.count(filename)) return fail(409, filename + " is being uploaded already");
  if (s_.delivery_.busy() && s_.delivery_.progress().name == filename)
    return fail(409, filename + " is being copied to the printer; try again when it is done");
  std::string err;
  writer_ = s_.lib_.create(filename, err);
  if (!writer_) return fail(500, err);
  name_ = filename;
  s_.uploading_.insert(name_);
  in_file_ = true;
  return true;
}

bool Service::Upload::data(const uint8_t* d, size_t n) {
  if (in_file_ && cfg_) {
    if (cfg_text_.size() + n > kMaxConfig) return fail(413, "halyard.cfg is too large");
    cfg_text_.append((const char*)d, n);
    return true;
  }
  if (in_file_) return writer_->write(d, n) || fail(500, writer_->error());
  if (value_.size() + n > 1024) return fail(400, "form field " + field_ + " is too long");
  value_.append((const char*)d, n);
  return true;
}

bool Service::Upload::part_end() {
  if (in_file_ && cfg_) {
    in_file_ = false;
    stored_ = true;
    return true;
  }
  if (in_file_) {
    in_file_ = false;
    std::string err;
    if (!writer_->finish(err)) return fail(500, err);
    writer_.reset();
    stored_ = true;
    return true;
  }
  if (field_ == "print") print_ = value_ == "true";
  else if (field_ == "root") root_ = value_;
  else if (field_ == "path") path_ = value_;
  if (stored_ && !cfg_ && (root_ != "gcodes" || (!path_.empty() && path_ != "/"))) {
    s_.lib_.remove(name_);
    stored_ = false;
    return fail(400, root_ == "config"   ? config_refusal(name_)
                     : root_ != "gcodes" ? "uploads go to the gcodes root"
                                         : "Halyard's library has no folders");
  }
  return true;
}

bool Service::Upload::feed(const uint8_t* d, size_t n) {
  if (status_) return false;
  return parser_.feed(d, n) || fail(400, parser_.error());
}

void Service::Upload::finish(HttpResponse& out) {
  out = HttpResponse();
  if (!status_ && !parser_.done()) fail(400, "the upload ended early");
  if (!status_ && !stored_) fail(400, "no file in the upload");
  if (!status_ && cfg_) {
    std::string err;
    if (root_ != "config") fail(400, "halyard.cfg belongs in the config root (Machine in Mainsail)");
    else if (!path_.empty() && path_ != "/") fail(400, "the config root has no folders");
    else if (!s_.save_config(cfg_text_, err)) fail(500, err);
  }
  if (!name_.empty() && !cfg_) s_.uploading_.erase(name_);
  if (status_) {
    writer_.reset();
    out.status = status_;
    JsonDocument e;
    if (octoprint_) {
      e["error"] = error_;
    } else {
      e["error"]["code"] = status_;
      e["error"]["message"] = error_;
    }
    out.body = to_json(e);
    return;
  }
  if (cfg_) {
    out.status = 201;
    JsonDocument r;
    if (octoprint_) {
      r["done"] = true;
    } else {
      JsonObject res = r.to<JsonObject>();
      s_.config_info(res["item"].to<JsonObject>(), true);
      res["item"]["root"] = "config";
      res["action"] = "create_file";
    }
    out.body = to_json(r);
    return;
  }
  s_.file_changed("create_file", name_, "");
  bool started = false;
  if (print_) {
    std::string err;
    started = s_.hand_off(name_, true, err);
    if (!started) s_.respond("!! " + name_ + " is uploaded but not started: " + err);
  }
  out.status = 201;
  JsonDocument r;
  if (octoprint_) {
    r["files"]["local"]["name"] = name_;
    r["files"]["local"]["origin"] = "local";
    r["done"] = true;
    r["effectivePrint"] = started;
    r["effectiveSelect"] = started;
  } else {
    // Not wrapped in "result", unlike Moonraker's other routes: Mainsail 2.19 reads
    // response.data.item.path and reports "Cannot upload file!" otherwise.
    JsonObject res = r.to<JsonObject>();
    s_.file_info(name_, res["item"].to<JsonObject>(), true);
    res["item"]["root"] = "gcodes";
    res["print_started"] = started;
    res["print_queued"] = false;
    res["action"] = "create_file";
  }
  out.body = to_json(r);
}

// ---- polling ---------------------------------------------------------------------------

void Service::poll(uint32_t now_ms) {
  now_ = now_ms;
  if (reboot_at_ && (int32_t)(now_ - reboot_at_) >= 0 && platform_.reboot) {
    if (db_dirty_) db_save();  // a setting changed in the last 2 s is not lost
    platform_.reboot();
  }

  std::string k = klippy_state();
  if (k != "ready") probe_.starting = probe_.active = false;
  if (k != last_klippy_) {
    if (k == "ready") {
      notify("notify_klippy_ready", JsonVariantConst());
      volume_asked_ = false;
      // A new connection: the printer may have restarted, so nothing is known to be homed.
      homed_.clear();
      stepper_timeout_ms_ = kStepperTimeoutMs;
      z_adjust_ = 0;
      lin_advance_ = pos_poll_out_ = limits_known_ = factors_query_out_ = false;
      speed_factor_ = extrude_factor_ = 1;
      last_pos_poll_ = now_ - kPositionPollMs;
      last_factors_poll_ = now_;  // asked below, with the other settings
      // Its card may have changed too: listed again once the printer is known to be idle.
      card_files_.clear();
      card_listed_ = false;
      card_stale_ = true;
      card_error_.clear();
      card_version_++;
      settings_stale_ = true;  // marlin.cfg: the last copy stays until it is read again
    } else if (last_klippy_ == "ready") {
      notify(k == "shutdown" ? "notify_klippy_shutdown" : "notify_klippy_disconnected",
             JsonVariantConst());
    }
    last_klippy_ = k;
  }
  // The printer's size from its soft endstops, for Mainsail's G-code viewer and limits.
  if (k == "ready" && !volume_asked_ && !printer_.uploading()) {
    volume_asked_ = true;
    printer_.send_raw("M211", [this](const Reply& r) {
      for (auto& l : r.lines) {
        const char* mn = strstr(l.c_str(), "Min:");
        const char* mx = strstr(l.c_str(), "Max:");
        double a[3], b[3];
        if (mn && mx && sscanf(mn, "Min: X%lf Y%lf Z%lf", &a[0], &a[1], &a[2]) == 3 &&
            sscanf(mx, "Max: X%lf Y%lf Z%lf", &b[0], &b[1], &b[2]) == 3) {
          for (int i = 0; i < 3; i++) {
            axis_min_[i] = a[i];
            axis_max_[i] = b[i];
          }
        }
      }
    });
    ask_advance();
    ask_limits();
    ask_factors();
  }
  // The speed and flow factors, which the printer's Tune menu changes too. During a print
  // the reply waits behind the job's queued lines, a few seconds late.
  if (k == "ready" && !factors_query_out_ && !pid_running_ && !printer_.uploading() &&
      now_ - last_factors_poll_ >= kFactorsPollMs)
    ask_factors();
  // The position, for the toolhead panel. Not while printing: the reply would wait behind
  // the job's queued moves, and Mainsail shows no jog controls then anyway.
  if (k == "ready" && !pos_poll_out_ && scripts_running_ == 0 && !job_running() &&
      !pid_running_ && !printer_.uploading() && now_ - last_pos_poll_ >= kPositionPollMs) {
    last_pos_poll_ = now_;
    pos_poll_out_ = printer_.send_raw("M114", [this](const Reply& r) {
      pos_poll_out_ = false;
      for (auto& l : r.lines) parse_position(l, pos_);
    });
  }

  track_job();
  // Marlin switches idle steppers off (DISABLE_IDLE_*), without a word, after
  // DEFAULT_STEPPER_TIMEOUT_SEC of no motion: 120 s unless M18/M84 S changed it. An axis
  // that was off may have been moved by hand, so Halyard forgets the homing then, as
  // Klipper's idle timeout does. The clock starts at the move's ok, before the move ends,
  // so Halyard forgets first. M109 and M190 keep the steppers on while they wait.
  if (job_running() || heat_waits_ > 0) last_motion_ = now_;
  else if (!homed_.empty() && stepper_timeout_ms_ && now_ - last_motion_ >= stepper_timeout_ms_)
    homed_.clear();

  // The printer's card, when something may have changed it. Never during a print: M20
  // makes the printer read its card's directory in the middle of the job.
  if (card_stale_ && !card_list_out_) {
    std::string why;
    if (card_ready(why)) card_list();
  }

  if (now_ - last_temp_sample_ >= kTempSampleMs) {
    last_temp_sample_ = now_;
    const Status& s = printer_.status();
    float v[4] = {s.hotend, s.hotend_target, s.bed, s.bed_target};
    for (int i = 0; i < 4; i++) temps_[i][temp_head_] = v[i];
    powers_[0][temp_head_] = (uint8_t)std::lround(s.hotend_power * 100);
    powers_[1][temp_head_] = (uint8_t)std::lround(s.bed_power * 100);
    temp_head_ = (temp_head_ + 1) % kTempSamples;
    if (temp_count_ < kTempSamples) temp_count_++;
  }

  // Marlin's settings, for marlin.cfg: once the printer is otherwise quiet, since M503's
  // dozens of lines must not land in a console script's reply.
  if (k == "ready" && settings_stale_ && !settings_query_out_ && scripts_running_ == 0 &&
      !pid_running_ && !printer_.uploading())
    ask_settings();

  // Halyard's own temperature, for Mainsail's Machine page, which shows the last one sent.
  if (now_ - last_proc_stat_ >= kProcStatMs || last_proc_stat_ == 0) {
    last_proc_stat_ = now_ ? now_ : 1;
    if (platform_.cpu_temp) {
      cpu_temp_ = platform_.cpu_temp();
      cpu_temp_known_ = std::isfinite(cpu_temp_);
    }
    if (!conns_.empty()) {
      JsonDocument d;
      JsonObject p = d.to<JsonArray>().add<JsonObject>();
      if (cpu_temp_known_) p["cpu_temp"] = round2(cpu_temp_);
      else p["cpu_temp"] = nullptr;
      p["system_cpu_usage"]["cpu"] = 0;
      p["system_memory"]["available"] = platform_.free_memory ? platform_.free_memory() / 1024 : 0;
      p["websocket_connections"] = (int)conns_.size();
      notify("notify_proc_stat_update", d.as<JsonVariantConst>());
    }
  }

  if (now_ - last_status_push_ >= kStatusPushMs) {
    last_status_push_ = now_;
    push_status_updates();
  }
  screen_poll();

  if (db_dirty_ && now_ - db_changed_ >= kDbSaveMs) db_save();
}

// ---- the job, as Mainsail's print_stats shows it ---------------------------------------

void Service::track_job() {
  const Status& st = printer_.status();
  double dt = (now_ - last_track_) / 1000.0;
  last_track_ = now_;
  Job j = st.job;
  if (pstate_ == PrintState::Printing && j == Job::Printing) print_duration_ += dt;
  // Unknown is the link reconnecting: keep what was known until the printer says.
  if (j != last_job_ && j != Job::Unknown) {
    Job was = last_job_;
    last_job_ = j;
    bool was_active = was == Job::Printing || was == Job::Paused || was == Job::Cancelling;
    if (j == Job::Printing) homed_ = "xyz";  // a print homes first
    // A finished print switches the steppers off (SD_FINISHED_RELEASECOMMAND), and so does
    // Halyard after a cancel: the axes may be moved by hand.
    if (j == Job::Idle && was_active) homed_.clear();
    if (j == Job::Printing && !was_active) {
      // A hand-off began its job already; anything else (the printer's own menu, a print
      // running when Halyard connected) starts one here.
      if (pstate_ != PrintState::Printing || !job_in_history_)
        begin_job(library_name_for(st.file));
      message_.clear();
    } else if (j == Job::Paused) {
      pstate_ = PrintState::Paused;
    } else if (j == Job::Printing && was == Job::Paused) {
      pstate_ = PrintState::Printing;
    } else if (j == Job::Idle && was_active) {
      end_job(st.prints_done != last_prints_done_ ? PrintState::Complete : PrintState::Cancelled,
              "");
    }
  }
  last_prints_done_ = st.prints_done;
  // A print that was running when Halyard connected (after an update, say) is seen before
  // M27 C has named its file.
  if (job_file_.empty() && !st.file.empty() && (pstate_ == PrintState::Printing || pstate_ == PrintState::Paused)) {
    job_file_ = library_name_for(st.file);
    if (job_in_history_ && !history_.empty()) {
      history_.front().filename = job_file_;
      history_meta(history_.front());
    }
  }
}

void Service::begin_job(const std::string& filename) {
  if (job_in_history_) end_job(PrintState::Cancelled, "");
  job_file_ = filename;
  pstate_ = PrintState::Printing;
  message_.clear();
  job_start_ = uptime();
  print_duration_ = final_total_ = final_progress_ = final_filament_ = 0;
  HistoryJob h;
  char id[8];
  snprintf(id, sizeof id, "%06X", next_job_id_++);
  h.job_id = id;
  h.filename = filename;
  h.status = "in_progress";
  h.start_time = wall();
  history_meta(h);
  history_.push_front(h);
  while (history_.size() > kHistoryMax) history_.pop_back();
  job_in_history_ = true;
  history_changed("added", h);
}

void Service::end_job(PrintState how, const std::string& message) {
  // The job's G-code may have changed them: slicers write M900, M203, M204, M220 and M221.
  if (lin_advance_) ask_advance();
  ask_limits();
  ask_factors();
  settings_stale_ = true;
  const Status& st = printer_.status();
  bool copying = handoff_active_ && handoff_print_;
  final_total_ = uptime() - job_start_;
  final_progress_ = how == PrintState::Complete ? 1
                    : !copying && st.sd_size ? (double)st.sd_pos / st.sd_size : 0;
  pstate_ = how;
  message_ = message;
  if (!job_in_history_) return;
  job_in_history_ = false;
  HistoryJob& h = history_.front();
  h.status = how == PrintState::Complete ? "completed"
             : how == PrintState::Cancelled ? "cancelled" : "error";
  h.end_time = wall();
  h.total_duration = uptime() - job_start_;
  h.print_duration = print_duration_;
  // Halyard doesn't count the extrusion as it prints (the printer reads the file, and
  // comments aren't on its copy), so a completed job used the slicer's total.
  if (how == PrintState::Complete && h.has_meta && h.meta.filament_total > 0)
    h.filament_used = final_filament_ = h.meta.filament_total;
  totals_.jobs++;
  totals_.time += h.total_duration;
  totals_.print_time += h.print_duration;
  totals_.filament += h.filament_used;
  totals_.longest_job = std::max(totals_.longest_job, h.total_duration);
  totals_.longest_print = std::max(totals_.longest_print, h.print_duration);
  history_changed("finished", h);
}

void Service::history_json(const HistoryJob& h, JsonObject o) {
  o["job_id"] = h.job_id;
  struct stat st;
  o["exists"] = stat(lib_.path(h.filename).c_str(), &st) == 0;
  if (h.status == "in_progress") o["end_time"] = nullptr;
  else o["end_time"] = round2(h.end_time);
  o["filament_used"] = round2(h.filament_used);
  o["filename"] = h.filename;
  JsonObject m = o["metadata"].to<JsonObject>();
  if (h.has_meta) {
    m["size"] = h.size;
    m["modified"] = h.modified;
    metadata_json(h.meta, m);
  }
  o["print_duration"] = round2(h.status == "in_progress" ? print_duration_ : h.print_duration);
  o["status"] = h.status;
  o["start_time"] = round2(h.start_time);
  o["total_duration"] =
      round2(h.status == "in_progress" ? uptime() - job_start_ : h.total_duration);
}

void Service::history_meta(HistoryJob& h) {
  LibraryEntry e;
  struct stat st;
  h.has_meta = lib_.get(h.filename, e) && stat(lib_.path(h.filename).c_str(), &st) == 0;
  if (!h.has_meta) return;
  h.meta = e.meta;
  h.size = (uint32_t)st.st_size;
  h.modified = (double)st.st_mtime;
}

void Service::history_changed(const char* action, const HistoryJob& h) {
  JsonDocument d;
  JsonObject p = d.to<JsonArray>().add<JsonObject>();
  p["action"] = action;
  history_json(h, p["job"].to<JsonObject>());
  notify("notify_history_changed", d.as<JsonVariantConst>());
}

std::string Service::library_name_for(const std::string& printer_file) {
  // Marlin reports "PHONES~1.GCO" (and the long name after it, if it knows one).
  std::string short_name = upper(printer_file.substr(0, printer_file.find(' ')));
  if (short_name.empty()) return "";
  for (auto& n : lib_.names()) {
    LibraryEntry e;
    if (lib_.get(n, e) && upper(e.printer_name) == short_name) return n;
  }
  size_t sp = printer_file.find(' ');
  return ascii(sp == std::string::npos ? printer_file : printer_file.substr(sp + 1));
}

bool Service::hand_off(const std::string& name, bool print, std::string& err) {
  if (link_.state() != LinkState::Ready) {
    err = "the printer is not connected";
    return false;
  }
  if (pid_running_ || probe_.starting || probe_.active) {
    err = pid_running_ ? "a PID autotune is running" : "CALIBRATE_Z_OFFSET is running";
    return false;
  }
  bool ok = delivery_.start(
      name, print,
      [this, name, print](bool ok2, const std::string& msg) {
        handoff_active_ = false;
        card_stale_ = true;  // written to, or a partial file removed
        if (ok2) {
          respond("// " + msg);
        } else if (cancel_requested_) {
          respond("// " + std::string(print ? "Print" : "Copy") + " of " + name + " cancelled");
          if (print) end_job(PrintState::Cancelled, "");
        } else {
          respond("!! " + std::string(print ? "Print" : "Copy") + " of " + name + " failed: " + msg);
          if (print) end_job(PrintState::Error, msg);
        }
        cancel_requested_ = false;
      },
      err);
  if (!ok) return false;
  handoff_active_ = true;
  handoff_print_ = print;
  cancel_requested_ = false;
  if (print) begin_job(name);
  respond(std::string("// ") + (print ? "Starting " : "Copying ") + name +
          (print ? ": preheat, copy to the printer's card, then print" : " to the printer's card"));
  return true;
}

void Service::delivery_event(const std::string& e) { respond("// " + e); }

// ---- printer objects -------------------------------------------------------------------

std::vector<std::string> Service::object_names() const {
  std::vector<std::string> v = {
      "webhooks",     "print_stats", "virtual_sdcard", "display_status", "idle_timeout",
      "pause_resume", "extruder",    "heater_bed",     "heaters",        "toolhead",
      "gcode_move",   "configfile",  "system_stats",   "motion_report", "halyard"};
  if (!link_.caps().firmware.empty()) v.push_back("mcu");
  for (auto& m : kMacros)
    if (!m.probe || has_probe()) v.push_back(std::string("gcode_macro ") + m.name);
  if (has_probe()) v.push_back("manual_probe");
  return v;
}

bool Service::build_object(const std::string& name, JsonObject o) {
  const Status& st = printer_.status();
  bool active = pstate_ == PrintState::Printing || pstate_ == PrintState::Paused;
  bool copying = handoff_active_ && handoff_print_;
  double progress = !active ? final_progress_
                    : !copying && st.sd_size ? (double)st.sd_pos / st.sd_size : 0;
  double total = active ? uptime() - job_start_ : final_total_;
  // Mainsail shows both messages at once. print_stats carries errors, as Klipper's does;
  // display_status carries the copy's progress.
  std::string display;
  if (copying) {
    Delivery::Progress p = delivery_.progress();
    char buf[96];
    if (p.total && strcmp(p.step, "uploading") == 0)
      snprintf(buf, sizeof buf, "Copying to the printer: %d%%", (int)(100.0 * p.bytes / p.total));
    else
      snprintf(buf, sizeof buf, "Preparing: %s", p.step);
    display = buf;
  }

  if (name == "webhooks") {
    std::string k = klippy_state();
    o["state"] = k;
    o["state_message"] = klippy_message(k);
  } else if (name == "print_stats") {
    o["filename"] = job_file_;
    o["total_duration"] = round2(total);
    o["print_duration"] = round2(print_duration_);
    o["filament_used"] = round2(final_filament_);
    o["state"] = state_name((int)pstate_);
    o["message"] = message_;
    JsonObject info = o["info"].to<JsonObject>();
    info["total_layer"] = nullptr;
    info["current_layer"] = nullptr;
  } else if (name == "virtual_sdcard") {
    o["file_path"] = job_file_.empty() ? "" : lib_.path(job_file_);
    o["progress"] = round2(progress * 100) / 100;
    o["is_active"] = pstate_ == PrintState::Printing && !copying;
    o["file_position"] = active && !copying ? st.sd_pos : 0;
    o["file_size"] = active && !copying ? st.sd_size : 0;
  } else if (name == "display_status") {
    o["progress"] = round2(progress * 100) / 100;
    o["message"] = display;
  } else if (name == "halyard") {
    // Halyard's own, for Halyard's Mainsail: the hand-off's progress (a copy, or a print's
    // copy before it starts), and the printer's card's version, which changes with every
    // listing (printer.card.list has the files).
    JsonObject h = o["handoff"].to<JsonObject>();
    if (delivery_.busy()) {
      Delivery::Progress p = delivery_.progress();
      h["state"] = p.step;
      h["filename"] = p.name;
      h["printer_name"] = p.printer_name;
      h["print"] = handoff_print_;
      h["bytes"] = p.bytes;
      h["total"] = p.total;
      h["elapsed"] = round2(p.upload_ms / 1000.0);
    } else {
      h["state"] = "idle";
    }
    o["card_version"] = card_version_;
  } else if (name == "motion_report") {
    // Marlin does not report its speed. Without the object Mainsail shows NaN for speed and
    // flow; with zeros it shows 0. The position is M114's, as the toolhead's.
    JsonArray pos = o["live_position"].to<JsonArray>();
    for (int i = 0; i < 4; i++) pos.add(round2(pos_[i] + (i == 2 ? z_adjust_ : 0)));
    o["live_velocity"] = 0;
    o["live_extruder_velocity"] = 0;
  } else if (name == "idle_timeout") {
    o["state"] = active ? "Printing" : "Ready";
    o["printing_time"] = round2(active ? uptime() - job_start_ : 0);
  } else if (name == "pause_resume") {
    o["is_paused"] = pstate_ == PrintState::Paused;
  } else if (name == "extruder") {
    o["temperature"] = round2(st.hotend);
    o["target"] = round2(st.hotend_target);
    o["power"] = round2(st.hotend_power);
    o["can_extrude"] = st.hotend >= kMinExtrudeTemp;
    // Only with Linear Advance: Halyard's Mainsail hides the panel without it. Marlin has
    // no smooth time, so that is never there.
    if (lin_advance_) o["pressure_advance"] = advance_k_;
  } else if (name == "heater_bed") {
    o["temperature"] = round2(st.bed);
    o["target"] = round2(st.bed_target);
    o["power"] = round2(st.bed_power);
  } else if (name == "heaters") {
    JsonArray h = o["available_heaters"].to<JsonArray>();
    h.add("extruder");
    h.add("heater_bed");
    JsonArray s = o["available_sensors"].to<JsonArray>();
    s.add("extruder");
    s.add("heater_bed");
    o["available_monitors"].to<JsonArray>();
  } else if (name == "toolhead") {
    // As Klipper's: the G-code position plus the babysteps.
    o["homed_axes"] = homed_axes();
    JsonArray pos = o["position"].to<JsonArray>();
    JsonArray mn = o["axis_minimum"].to<JsonArray>();
    JsonArray mx = o["axis_maximum"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
      pos.add(round2(pos_[i] + (i == 2 ? z_adjust_ : 0)));
      mn.add(round2(axis_min_[i]));
      mx.add(round2(axis_max_[i]));
    }
    pos.add(round2(pos_[3]));
    mn.add(0);
    mx.add(0);
    o["extruder"] = "extruder";
    o["print_time"] = 0;
    o["estimated_print_time"] = round2(uptime());
    // Only what Marlin has, and only once read: Halyard's Mainsail shows the Limits panel
    // with max_velocity, and no square corner velocity, accel_to_decel or cruise ratio.
    if (limits_known_) {
      o["max_velocity"] = round2(velocity_);
      o["max_accel"] = round2(accel_);
    }
  } else if (name == "gcode_move") {
    o["speed_factor"] = round2(speed_factor_);
    o["speed"] = 0;
    o["extrude_factor"] = round2(extrude_factor_);
    o["absolute_coordinates"] = true;
    o["absolute_extrude"] = true;
    for (const char* k : {"position", "gcode_position"}) {
      JsonArray a = o[k].to<JsonArray>();
      for (int i = 0; i < 4; i++) a.add(round2(pos_[i]));
    }
    // The Z babysteps. Only with babystepping: Halyard's Mainsail shows its Z offset
    // control when SET_GCODE_OFFSET is among the commands.
    JsonArray h = o["homing_origin"].to<JsonArray>();
    for (int i = 0; i < 4; i++) h.add(i == 2 ? std::round(z_adjust_ * 1000) / 1000 : 0);
  } else if (name == "configfile") {
    // Klipper's configuration, as far as Marlin reveals it. config holds strings,
    // settings the parsed values.
    char buf[64];
    JsonObject cfg = o["config"].to<JsonObject>();
    JsonObject set = o["settings"].to<JsonObject>();
    auto both = [&](const char* sec, const char* key, double v) {
      snprintf(buf, sizeof buf, "%g", v);
      cfg[sec][key] = std::string(buf);
      set[sec][key] = v;
    };
    both("extruder", "min_temp", 0);
    both("extruder", "max_temp", kMaxHotend);
    both("extruder", "min_extrude_temp", kMinExtrudeTemp);
    both("extruder", "filament_diameter", 1.75);
    both("heater_bed", "min_temp", 0);
    both("heater_bed", "max_temp", kMaxBed);
    const char* axes[] = {"stepper_x", "stepper_y", "stepper_z"};
    for (int i = 0; i < 3; i++) {
      both(axes[i], "position_min", axis_min_[i]);
      both(axes[i], "position_max", axis_max_[i]);
    }
    // The Z offset control saves into the probe's offset (Z_OFFSET_APPLY_PROBE) when Z
    // "homes with the probe"; Marlin has nothing else to keep babysteps in.
    if (has_probe()) {
      cfg["stepper_z"]["endstop_pin"] = "probe:z_virtual_endstop";
      set["stepper_z"]["endstop_pin"] = "probe:z_virtual_endstop";
    }
    if (lin_advance_) both("extruder", "pressure_advance", advance_start_);
    if (limits_known_) {
      both("printer", "max_velocity", velocity_start_);
      both("printer", "max_accel", accel_start_);
    }
    cfg["printer"]["kinematics"] = "cartesian";
    set["printer"]["kinematics"] = "cartesian";
    for (const char* sec : {"virtual_sdcard", "pause_resume", "display_status"}) {
      cfg[sec].to<JsonObject>();
      set[sec].to<JsonObject>();
    }
    // Mainsail warns ("Missing configuration") without these. Halyard runs them itself:
    // PAUSE, RESUME and CANCEL_PRINT are translated in the console.
    for (const char* m : {"pause", "resume", "cancel_print"}) {
      std::string sec = std::string("gcode_macro ") + m;
      std::string what = std::string("Halyard: ") + (m[0] == 'p' ? "M25" : m[0] == 'r' ? "M24" : "M524");
      cfg[sec]["gcode"] = what;
      set[sec]["gcode"] = what;
      set[sec]["description"] = what;
    }
    for (auto& m : kMacros) {
      if (m.probe && !has_probe()) continue;
      std::string sec = "gcode_macro " + lower(m.name);
      cfg[sec]["gcode"] = m.gcode;
      set[sec]["gcode"] = m.gcode;
    }
    cfg["virtual_sdcard"]["path"] = lib_.root();
    set["virtual_sdcard"]["path"] = lib_.root();
    // Mainsail shows SAVE CONFIG while this is true.
    o["save_config_pending"] = !save_pending_.empty();
    JsonObject items = o["save_config_pending_items"].to<JsonObject>();
    for (auto& kv : save_pending_) items[kv.first]["value"] = kv.second;
    o["warnings"].to<JsonArray>();
  } else if (name == "mcu") {
    // Marlin, on Mainsail's Machine page: its name and version from M115 ("Marlin 2.1.2.8
    // (Sep 21 2026 17:36:48)": app Marlin, version 2.1.2.8), the machine's name where
    // Klipper shows the chip, and M115's other fields and capabilities in the details.
    // Marlin reports no load or clock, so last_stats is left out.
    const Caps& c = link_.caps();
    if (c.firmware.empty()) return false;
    std::string fw = ascii(c.firmware), app = fw, version;
    size_t sp = fw.find(' ');
    if (sp != std::string::npos) {
      app = fw.substr(0, sp);
      version = fw.substr(sp + 1);
      version = trim(version.substr(0, version.find(" (")));
    }
    o["app"] = app;
    o["mcu_version"] = version;
    JsonObject k = o["mcu_constants"].to<JsonObject>();
    auto it = c.info.find("MACHINE_TYPE");
    if (it != c.info.end()) k["MCU"] = ascii(it->second);
    k["FIRMWARE_NAME"] = fw;
    for (auto& kv : c.info)
      if (kv.first != "MACHINE_TYPE" && kv.first != "FIRMWARE_NAME" && kv.first != "UUID")
        k[kv.first] = ascii(kv.second);
    for (auto& kv : c.cap) k["Cap:" + kv.first] = kv.second ? 1 : 0;
  } else if (name == "system_stats") {
    o["sysload"] = 0;
    o["cputime"] = round2(uptime());
    o["memavail"] = platform_.free_memory ? platform_.free_memory() / 1024 : 0;
  } else if (starts_with(name, "gcode_macro ")) {  // a macro's variables: none
    bool known = false;
    for (auto& m : kMacros)
      known |= lower(name.substr(12)) == lower(m.name) && (!m.probe || has_probe());
    return known;
  } else if (name == "manual_probe") {
    if (!has_probe()) return false;
    o["is_active"] = probe_.active;
    o["z_position"] = std::round(probe_.z * 1000) / 1000;
    const std::vector<double>& t = probe_.tried;
    auto up = std::upper_bound(t.begin(), t.end(), probe_.z);
    auto low = std::lower_bound(t.begin(), t.end(), probe_.z);
    if (probe_.active && low != t.begin()) o["z_position_lower"] = *(low - 1);
    else o["z_position_lower"] = nullptr;
    if (probe_.active && up != t.end()) o["z_position_upper"] = *up;
    else o["z_position_upper"] = nullptr;
  } else if (name == "gcode") {  // queried for console completion, never listed
    JsonObject cmds = o["commands"].to<JsonObject>();
    for (auto& c : kCommands)
      if (command_available(c[0])) cmds[c[0]]["help"] = c[1];
  } else {
    return false;
  }
  return true;
}

void Service::objects_status(JsonVariantConst objects, JsonObject status, Conn* remember) {
  if (remember) {
    remember->subs.clear();
    remember->last.clear();
  }
  if (!objects.is<JsonObjectConst>()) return;
  for (JsonPairConst kv : objects.as<JsonObjectConst>()) {
    std::string name = kv.key().c_str();
    JsonDocument full;
    JsonObject o = full.to<JsonObject>();
    if (!build_object(name, o)) continue;
    std::vector<std::string> fields;
    if (kv.value().is<JsonArrayConst>())
      for (JsonVariantConst f : kv.value().as<JsonArrayConst>()) fields.push_back(str(f));
    JsonObject dst = status[name].to<JsonObject>();
    for (JsonPair p : o) {
      std::string f = p.key().c_str();
      if (!fields.empty() && std::find(fields.begin(), fields.end(), f) == fields.end()) continue;
      JsonVariantConst v = p.value();
      dst[f] = v;
      if (remember) remember->last[name + "." + f] = to_json(v);
    }
    if (remember) remember->subs[name] = fields;
  }
}

void Service::push_status_updates() {
  for (auto& kv : conns_) {
    Conn& c = kv.second;
    if (c.subs.empty()) continue;
    JsonDocument d;
    JsonArray params = d.to<JsonArray>();
    JsonObject diff = params.add<JsonObject>();
    bool any = false;
    for (auto& sub : c.subs) {
      JsonDocument full;
      JsonObject o = full.to<JsonObject>();
      if (!build_object(sub.first, o)) continue;
      for (JsonPair p : o) {
        std::string f = p.key().c_str();
        if (!sub.second.empty() &&
            std::find(sub.second.begin(), sub.second.end(), f) == sub.second.end())
          continue;
        JsonVariantConst v = p.value();
        std::string s = to_json(v);
        std::string& last = c.last[sub.first + "." + f];
        if (s == last) continue;
        last = s;
        diff[sub.first][f] = v;
        any = true;
      }
    }
    if (!any) continue;
    params.add(round2(uptime()));
    notify("notify_status_update", d.as<JsonVariantConst>(), kv.first);
  }
}

// ---- the console -----------------------------------------------------------------------

struct Service::Script {
  Call call;
  int outstanding = 0;
  bool all_sent = false;
  std::string error;
};

void Service::store_gcode(const std::string& message, bool command) {
  gcode_store_.push_back({message, wall(), command});
  while (gcode_store_.size() > kGcodeStoreMax) gcode_store_.pop_front();
}

void Service::respond(const std::string& text) {
  std::string line = ascii(text);
  if (on_console) on_console(line);
  store_gcode(line, false);
  JsonDocument d;
  d.add(line);
  notify("notify_gcode_response", d.as<JsonVariantConst>());
}

void Service::printer_line(const std::string& line) {
  // While a console script runs, its replies are shown from the script's own callbacks.
  if (scripts_running_ > 0) return;
  std::string t = trim(line);
  if (!is_ascii(t)) return;
  // The answer to Halyard's own M900 question.
  if (advance_query_out_ && (t.find("\"M900\"") != std::string::npos ||
                             starts_with(t, "echo:Advance")))
    return;
  // And to its M503 question, for marlin.cfg: "echo:; Steps per unit:", "echo:  M92 ...".
  if (settings_query_out_ && (starts_with(t, "echo:;") || starts_with(t, "echo: ") ||
                              t.find("\"M503\"") != std::string::npos))
    return;
  // And to its M221 question ("echo:E0 Flow: 100%"; M220's "FR:100%" has no "echo:").
  if (factors_query_out_ && starts_with(t, "echo:E") && t.find(" Flow: ") != std::string::npos)
    return;
  bool forward = starts_with(t, "Error:") || starts_with(t, "!!") || starts_with(t, "//action:") ||
                 (starts_with(t, "echo:") && !starts_with(t, "echo:busy") &&
                  t.find("Soft endstops") == std::string::npos) ||
                 starts_with(t, "Done printing file");
  if (forward) respond(console_text(t));
}

// A calibration result is in use at once. With an EEPROM, SAVE CONFIG (M500) keeps it, as in
// Klipper. Stock Marlin has no EEPROM_SETTINGS (no Cap:EEPROM, no M500): the result lasts
// until the printer restarts, and SAVE CONFIG doesn't show.
std::string Service::keep_until_saved(const std::string& item, const std::string& value) {
  if (!link_.caps().has("EEPROM"))
    return " The printer has no EEPROM (EEPROM_SETTINGS), so this lasts until it restarts.";
  save_pending_[item] = value;
  return " Press SAVE CONFIG to keep it (M500).";
}

void Service::script_line_done(const std::shared_ptr<Script>& s, const std::string& err) {
  s->outstanding--;
  if (!err.empty() && s->error.empty()) s->error = err;
  if (s->outstanding > 0 || !s->all_sent) return;
  scripts_running_--;
  last_pos_poll_ = now_ - kPositionPollMs;  // where the script left the toolhead
  JsonDocument r;
  r.set("ok");
  reply(s->call, s->error.empty() ? 0 : kBadRequest, r, s->error);
}

// One console line. Klipper commands Mainsail sends are translated; G-code goes to the
// printer. False with `err` if it cannot run; otherwise script_line_done() follows once.
bool Service::run_line(const std::shared_ptr<Script>& s, const std::string& line,
                       std::string& err) {
  std::vector<std::string> words = words_of(line);
  std::string cmd = upper(words[0]);
  auto done_fn = [this, s](bool ok, const std::string& msg) {
    if (ok) respond("// " + msg);
    else respond("!! " + msg);
    script_line_done(s, ok ? "" : msg);
  };
  auto send = [this, s](const std::string& gcode, std::string& err2) {
    return script_send(s, gcode, err2);
  };

  if (!is_gcode_word(cmd)) {
    if (cmd == "SET_HEATER_TEMPERATURE") {
      std::string heater, target;
      if (!klipper_arg(words, "HEATER", heater)) return err = "missing HEATER", false;
      double t = klipper_arg(words, "TARGET", target) ? strtod(target.c_str(), nullptr) : 0;
      char buf[32];
      if (heater == "extruder") snprintf(buf, sizeof buf, "M104 S%g", t);
      else if (heater == "heater_bed") snprintf(buf, sizeof buf, "M140 S%g", t);
      else return err = "The value '" + heater + "' is not valid for HEATER", false;
      return send(buf, err);
    }
    if (cmd == "TURN_OFF_HEATERS") return send("M104 S0", err) && send("M140 S0", err);
    if (cmd == "BED_PID_TUNE" || cmd == "EXTRUDER_PID_TUNE" || cmd == "PID_CALIBRATE") {
      std::string heater = cmd == "BED_PID_TUNE" ? "heater_bed" : "extruder", target;
      if (cmd == "PID_CALIBRATE" && !klipper_arg(words, "HEATER", heater))
        return err = "missing HEATER", false;
      double t = heater == "heater_bed" ? kBedPidTarget : kHotendPidTarget;
      if (klipper_arg(words, "TARGET", target)) t = strtod(target.c_str(), nullptr);
      else if (cmd == "PID_CALIBRATE") return err = "missing TARGET", false;
      return pid_tune(s, heater, t, err);
    }
    if (cmd == "CALIBRATE_Z_OFFSET" || cmd == "PROBE_CALIBRATE") return z_calibrate(s, err);
    if (cmd == "TESTZ") {
      std::string z;
      if (!klipper_arg(words, "Z", z)) return err = "missing Z", false;
      return z_test(s, z, err);
    }
    if (cmd == "ACCEPT" || cmd == "ABORT") return z_finish(s, cmd == "ACCEPT", err);
    if (cmd == "SAVE_GCODE_STATE" || cmd == "RESTORE_GCODE_STATE") {
      // Mainsail wraps its jog and extrude moves in these. Marlin cannot save G90/G91 or
      // M82/M83, so RESTORE puts back its defaults. Not during a print, not even paused:
      // that would change the job's own modes.
      if (job_running())
        return err = "not while a print is running or paused: Marlin cannot keep the print's "
                     "G90/G91 and M82/M83 (M600 changes filament)", false;
      if (cmd == "SAVE_GCODE_STATE") return true;
      return send("G90", err) && send("M82", err);
    }
    if (cmd == "SET_GCODE_OFFSET") return z_offset_adjust(s, words, err);
    if (cmd == "Z_OFFSET_APPLY_PROBE") return z_offset_apply_probe(s, err);
    if (cmd == "Z_OFFSET_APPLY_ENDSTOP")
      return err = "Marlin has no Z endstop position to keep babysteps in; with a probe, "
                   "Z_OFFSET_APPLY_PROBE puts them into its offset", false;
    if (cmd == "SET_PRESSURE_ADVANCE") return pressure_advance(s, words, err);
    if (cmd == "SET_VELOCITY_LIMIT") return velocity_limit(s, words, err);
    if (cmd == "SAVE_CONFIG") {
      if (save_pending_.empty()) {
        respond("// Nothing to save: no calibration has changed the printer's settings");
        return true;
      }
      return script_send(s, "M500", err, [this](const Reply&, std::string& e) {
        if (!e.empty()) return;
        save_pending_.clear();
        respond("// Saved to the printer's EEPROM (M500)");
      });
    }
    if (cmd == "PAUSE" || cmd == "RESUME" || cmd == "CANCEL_PRINT") {
      s->outstanding++;
      JsonDocument none;
      Call c;  // answered through done_fn, not as a JSON-RPC reply
      std::string e;
      if (cmd == "PAUSE") printer_.pause(done_fn);
      else if (cmd == "RESUME") printer_.resume(done_fn);
      else if (m_print_cancel(c, none.as<JsonVariantConst>(), none, e) != kDeferred)
        script_line_done(s, e);
      return true;
    }
    if (cmd == "SDCARD_RESET_FILE") {
      if (pstate_ == PrintState::Printing || pstate_ == PrintState::Paused)
        return err = "cannot reset the file while printing", false;
      pstate_ = PrintState::Standby;
      job_file_.clear();
      message_.clear();
      print_duration_ = final_total_ = final_progress_ = final_filament_ = 0;
      return true;
    }
    if (cmd == "FIRMWARE_RESTART" || cmd == "RESTART") {
      if (delivery_.busy()) delivery_.abort();
      if (on_restart) on_restart();
      link_.reconnect();
      return true;
    }
    if (cmd == "HELP") {
      for (auto& c : kCommands)
        if (command_available(c[0])) respond(std::string(c[0]) + ": " + c[1]);
      return true;
    }
    return err = "Unknown command:\"" + cmd + "\"", false;
  }
  // Commands that belong to Halyard's hand-off. M28 during a media print would abort it.
  if (cmd == "M28" || cmd == "M29")
    return err = cmd + " is Halyard's: upload the file instead, and it is copied on start", false;
  if ((cmd == "M21" || cmd == "M22") &&
      (printer_.status().job == Job::Printing || printer_.status().job == Job::Paused))
    return err = cmd + " would unmount the card the printer is printing from", false;
  if (cmd == "M112") {
    std::string e;
    JsonDocument none;
    m_emergency_stop(Call(), none.as<JsonVariantConst>(), none, e);
    return true;
  }
  return send(line, err);
}

bool Service::script_send(const std::shared_ptr<Script>& s, const std::string& gcode,
                          std::string& err, Check check) {
  std::string code = upper(gcode.substr(0, gcode.find(' ')));
  s->outstanding++;
  bool heat_wait = code == "M109" || code == "M190";
  if (heat_wait) heat_waits_++;
  bool ok = printer_.send_raw(gcode, [this, s, code, gcode, check, heat_wait](const Reply& r) {
    if (heat_wait) heat_waits_--;
    std::string e;
    for (auto& l : r.lines) {
      bool report = code == "M105" || code == "M27";
      if (report || console_worthy(l)) respond(console_text(l));
      if (starts_with(trim(l), "Error:") && e.empty()) e = trim(l).substr(6);
    }
    if (r.ok_line.find("T:") != std::string::npos) respond(trim(r.ok_line));
    if (r.outcome == Outcome::Disconnected) e = "the printer disconnected";
    else if (r.outcome == Outcome::Lost && e.empty()) e = "no reply to " + code;
    if (e.empty()) {  // what Halyard mirrors for Mainsail
      if (is_code(code, "G28")) homed_ = merge_axes(homed_, homed_by(upper(gcode)));
      if (keeps_steppers_on(code)) last_motion_ = now_;
      if (is_code(code, "M18") || is_code(code, "M84")) steppers_off(upper(gcode));
      if (code == "M114")
        for (auto& l : r.lines) parse_position(l, pos_);
      if (code == "M900") {
        double k;
        std::string ks = upper(gcode);
        if (parse_advance(r.lines, k)) advance_k_ = k;
        else if (lin_advance_ && ks.find('K') != std::string::npos && ks.find('T') == std::string::npos)
          advance_k_ = strtod(ks.c_str() + ks.find('K') + 1, nullptr);
      }
      // Limits set in the console, or settings loaded or reset, show in the Limits panel.
      if (code == "M201" || code == "M203" || code == "M204" || code == "M501" || code == "M502")
        ask_limits();
      if ((code == "M501" || code == "M502") && lin_advance_) ask_advance();
      // Mainsail's speed and flow sliders send these: what Marlin made of them.
      if (code == "M220" || code == "M221") ask_factors();
      // marlin.cfg: a command M503 reports changed a setting; loading, resetting, saving,
      // a PID tune (M303 U1) and a mesh change several.
      std::string c = code_of(gcode);
      if (settings_codes_.count(c) || c == "M500" || c == "M501" || c == "M502" ||
          c == "M303" || c == "G29" || c == "M290")
        settings_stale_ = true;
    }
    if (check) check(r, e);
    script_line_done(s, e);
  });
  if (!ok) {
    s->outstanding--;
    if (heat_wait) heat_waits_--;
    err = printer_.uploading() ? "Halyard is copying a file to the printer; try again when it is done"
                               : "cannot send \"" + gcode + "\"";
  }
  return ok;
}

// ---- the toolhead and extruder panels ----------------------------------------------------

bool Service::command_available(const std::string& name) const {
  if (name == "SET_PRESSURE_ADVANCE") return lin_advance_;
  if (name == "SET_VELOCITY_LIMIT") return limits_known_;
  if (name == "SET_GCODE_OFFSET") return babystepping();
  if (name == "Z_OFFSET_APPLY_PROBE") return babystepping() && has_probe();
  if (name == "SAVE_CONFIG" || name == "M500") return link_.caps().has("EEPROM");
  return true;
}

bool Service::job_running() const {
  Job j = printer_.status().job;
  return pstate_ == PrintState::Printing || pstate_ == PrintState::Paused || handoff_active_ ||
         j == Job::Printing || j == Job::Paused;
}

// M18/M84: S sets Marlin's idle timeout (0: none) and switches nothing off.
void Service::steppers_off(const std::string& gcode) {
  double sec;
  if (m84_timeout(m84_args(gcode), sec)) stepper_timeout_ms_ = (uint32_t)(sec * 1000);
  homed_ = without_axes(homed_, switched_off(gcode));
}

// Marlin moves an unhomed axis wherever it is told: its software endstops apply only to
// homed axes, so a jog runs into the frame. Klipper refuses such a move, and so does
// Halyard: the whole script, before any of it is sent, since its lines go out together
// (a G28 in it counts for the lines after it) and Mainsail's jog would otherwise be left
// in G91. A print's own moves never come this way.
bool Service::moves_homed(const std::string& script, std::string& err) const {
  std::string homed = homed_axes();
  for (auto& raw : split(script, '\n')) {
    std::string line = upper(trim(raw));
    std::string code = line.substr(0, line.find_first_of(" \t;"));
    if (is_code(code, "G28")) homed = merge_axes(homed, homed_by(line));
    if (is_code(code, "M18") || is_code(code, "M84")) homed = without_axes(homed, switched_off(line));
    std::string unhomed;
    for (char a : axes_moved(line))
      if (homed.find(a) == std::string::npos) unhomed += (char)toupper((unsigned char)a);
    if (!unhomed.empty())
      return err = "Must home axis first: " + unhomed + " (" + trim(raw) +
                   "). Halyard forgets the homing when the steppers go off: M84, the end "
                   "of a print, or " + std::to_string(stepper_timeout_ms_ / 1000) +
                   " s without motion.", false;
  }
  return true;
}

std::string Service::homed_axes() const {
  Job j = printer_.status().job;
  return j == Job::Printing || j == Job::Paused ? "xyz" : homed_;
}

// SET_GCODE_OFFSET, as Mainsail's Z offset control sends it, as babysteps (M290): they move
// the nozzle at once, mid-print too. P0 keeps them out of the probe's offset on printers
// built with BABYSTEP_ZPROBE_OFFSET, so every Marlin behaves alike: babysteps last until
// the printer restarts, unless Z_OFFSET_APPLY_PROBE adds them to M851.
bool Service::z_offset_adjust(const std::shared_ptr<Script>& s,
                              const std::vector<std::string>& words, std::string& err) {
  if (!babystepping()) return err = "the printer has no babystepping (Cap:BABYSTEPPING in M115)", false;
  double d = 0;
  bool have = false;
  for (size_t i = 1; i < words.size(); i++) {
    size_t eq = words[i].find('=');
    std::string key = upper(words[i].substr(0, eq));
    if (key == "MOVE" || key == "MOVE_SPEED") continue;  // babysteps always move
    if (key != "Z" && key != "Z_ADJUST") return err = key + ": Marlin babysteps only Z", false;
    const char* a = eq == std::string::npos ? "" : words[i].c_str() + eq + 1;
    char* end;
    double v = strtod(a, &end);
    if (end == a) return err = "a number is needed: " + words[i], false;
    d = key == "Z" ? v - z_adjust_ : v;
    have = true;
  }
  if (!have) return err = "missing Z_ADJUST or Z", false;
  d = std::round(d * 1000) / 1000;
  if (d == 0) return true;
  if (std::fabs(d) > 2) return err = "at most 2 mm at a time (M290)", false;
  if (homed_axes().find('z') == std::string::npos)
    return err = "home Z first: Marlin babysteps only a homed axis", false;
  return script_send(s, "M290 P0 Z" + fmt3(d), err, [this, d](const Reply&, std::string& e) {
    if (e.empty()) z_adjust_ = std::round((z_adjust_ + d) * 1000) / 1000;
  });
}

// Z_OFFSET_APPLY_PROBE: the babysteps go into the probe's Z offset (M851), which the next
// homing or probing uses, and SAVE_CONFIG keeps it (M500), as after CALIBRATE_Z_OFFSET.
// The nozzle is already where the babysteps put it, so the offset shown goes back to 0.
bool Service::z_offset_apply_probe(const std::shared_ptr<Script>& s, std::string& err) {
  if (!has_probe()) return err = "the printer reports no Z probe (Cap:Z_PROBE in M115)", false;
  if (z_adjust_ == 0) {
    respond("// Nothing to apply: the Z offset is 0");
    return true;
  }
  double adjust = z_adjust_;
  return script_send(s, "M851", err, [this, s, adjust](const Reply& r, std::string& e) {
    if (!e.empty()) return;
    double old = 0;
    bool found = false;
    for (auto& l : r.lines) {
      size_t at = l.find("M851");  // "  M851 X-33.00 Y-15.00 Z-0.90 ; (mm)"
      found |= at != std::string::npos && number_after(l.substr(at), " Z", old);
    }
    if (!found) {
      e = "cannot read the probe's Z offset (M851)";
      return;
    }
    // Marlin moves the probe's offset the way it babysteps: up is +.
    double offset = std::round((old + adjust) * 1000) / 1000;
    script_send(s, "M851 Z" + fmt3(offset), e,
                [this, adjust, old, offset](const Reply&, std::string& e2) {
                  if (!e2.empty()) return;
                  z_adjust_ = std::round((z_adjust_ - adjust) * 1000) / 1000;
                  respond("// Probe Z offset " + fmt3(offset) + " (it was " + fmt3(old) + ")." +
                          keep_until_saved("probe", "z_offset " + fmt3(offset)));
                });
  });
}

// SET_PRESSURE_ADVANCE as Linear Advance (M900 K). Both are mm of filament per mm/s of
// extrusion speed (Mainsail calls it seconds), so a value means the same in either.
bool Service::pressure_advance(const std::shared_ptr<Script>& s,
                               const std::vector<std::string>& words, std::string& err) {
  if (!lin_advance_)
    return err = "the printer has no Linear Advance (M900; LIN_ADVANCE in Marlin's configuration)", false;
  std::string ex, v;
  if (klipper_arg(words, "EXTRUDER", ex) && lower(ex) != "extruder")
    return err = "The value '" + ex + "' is not valid for EXTRUDER", false;
  if (klipper_arg(words, "SMOOTH_TIME", v)) return err = "Marlin's Linear Advance has no smooth time", false;
  if (!klipper_arg(words, "ADVANCE", v)) return err = "missing ADVANCE", false;
  char* end;
  double k = strtod(v.c_str(), &end);
  if (end == v.c_str() || k < 0 || k > 10) return err = "ADVANCE must be 0 to 10 (M900 K)", false;
  return script_send(s, "M900 K" + fmt3(k), err);  // script_send notes the new K
}

// Whether the printer has Linear Advance, and its K: M900 without arguments only reports.
// Its reply stays out of the console (printer_line).
void Service::ask_advance() {
  if (advance_query_out_) return;
  advance_query_out_ = printer_.send_raw("M900", [this](const Reply& r) {
    advance_query_out_ = false;
    if (r.outcome != Outcome::Ok && r.outcome != Outcome::OkInferred) return;
    double k;
    bool first = !lin_advance_;
    lin_advance_ = parse_advance(r.lines, k);
    if (!lin_advance_) return;
    advance_k_ = k;
    if (first) advance_start_ = k;
  });
}

// SET_VELOCITY_LIMIT, from Mainsail's Limits panel, as Marlin's maximum feedrate (M203) and
// acceleration (M204). Klipper's limits are the toolhead's, Marlin's are per axis: VELOCITY
// sets X and Y, ACCEL printing and travel moves. Like Klipper's, they last until the printer
// restarts (M500 would keep them). Marlin has no square corner velocity (its junction
// deviation is a distance that depends on the acceleration), accel_to_decel or cruise ratio,
// and Halyard's Mainsail shows none of them.
bool Service::velocity_limit(const std::shared_ptr<Script>& s,
                             const std::vector<std::string>& words, std::string& err) {
  if (!limits_known_)
    return err = "cannot read the printer's limits: it did not report M203 and M204", false;
  std::string v;
  for (const char* k : {"SQUARE_CORNER_VELOCITY", "ACCEL_TO_DECEL", "MINIMUM_CRUISE_RATIO"})
    if (klipper_arg(words, k, v)) return err = std::string(k) + ": Marlin has no such limit", false;
  double want[2] = {0, 0};
  const char* keys[2] = {"VELOCITY", "ACCEL"};
  for (int i = 0; i < 2; i++) {
    if (!klipper_arg(words, keys[i], v)) continue;
    char* end;
    want[i] = strtod(v.c_str(), &end);
    if (end == v.c_str() || *end || !(want[i] > 0))
      return err = std::string(keys[i]) + " must be a number above 0", false;
  }
  if (!want[0] && !want[1]) {  // as Klipper's: without parameters, it reports
    char buf[64];
    snprintf(buf, sizeof buf, "// max_velocity: %g (M203 X Y)", velocity_);
    respond(buf);
    snprintf(buf, sizeof buf, "// max_accel: %g (M204 P T)", accel_);
    respond(buf);
    return true;
  }
  // script_send asks for the limits again after each change: what Marlin made of them.
  want_velocity_ = want[0];
  want_accel_ = want[1];
  char buf[64];
  if (want[0]) {
    snprintf(buf, sizeof buf, "M203 X%g Y%g", want[0], want[0]);
    if (!script_send(s, buf, err)) return false;
  }
  if (want[1]) {
    snprintf(buf, sizeof buf, "M204 P%g T%g", want[1], want[1]);
    if (!script_send(s, buf, err)) return false;
  }
  return true;
}

// The limits from their reports: M201, M203 and M204 without parameters report (Marlin 2.1
// does; older ones may not). A Marlin that doesn't report both M203 and M204 has no
// Limits panel. want_*_ are values just set, to say why Marlin kept a lower one. The replies
// stay out of the console: report lines have no "echo:" (printer_line).
void Service::ask_limits() {
  auto ok = [](const Reply& r) { return r.outcome == Outcome::Ok || r.outcome == Outcome::OkInferred; };
  printer_.send_raw("M201", [this, ok](const Reply& r) {
    double x, y;
    if (ok(r) && report_value(r.lines, "M201", " X", x) && report_value(r.lines, "M201", " Y", y))
      axis_accel_ = std::min(x, y);
  });
  printer_.send_raw("M203", [this, ok](const Reply& r) {
    double x, y;
    if (!ok(r)) return;
    velocity_ = report_value(r.lines, "M203", " X", x) && report_value(r.lines, "M203", " Y", y)
                    ? std::min(x, y) : 0;
    char buf[128];
    double want = want_velocity_;
    want_velocity_ = 0;
    if (velocity_ > 0 && want > velocity_ + 0.01) {
      snprintf(buf, sizeof buf, "// Marlin keeps the maximum feedrate at %g mm/s "
               "(LIMITED_MAX_FR_EDITING in its configuration)", velocity_);
      respond(buf);
    }
  });
  printer_.send_raw("M204", [this, ok](const Reply& r) {
    double p;
    if (!ok(r)) return;
    bool was = limits_known_;
    accel_ = report_value(r.lines, "M204", " P", p) ? p : 0;
    limits_known_ = velocity_ > 0 && accel_ > 0;
    if (limits_known_ && !was) {
      velocity_start_ = velocity_;
      accel_start_ = accel_;
    }
    char buf[128];
    double want = want_accel_;
    want_accel_ = 0;
    if (want > 0 && axis_accel_ > 0 && want > axis_accel_ + 0.01) {
      snprintf(buf, sizeof buf, "// Marlin's maximum acceleration (M201) holds X and Y to %g mm/s2",
               axis_accel_);
      respond(buf);
    }
  });
}

// Mainsail's speed and flow sliders read gcode_move's speed_factor and extrude_factor;
// Marlin's M220 and M221 without parameters report them ("FR:150%", "echo:E0 Flow: 95%",
// the first extruder's). The replies stay out of the console (printer_line). A Marlin that
// doesn't report leaves them at 100 %.
void Service::ask_factors() {
  if (printer_.uploading()) return;
  last_factors_poll_ = now_;
  factors_query_out_ = true;
  auto percent = [](const Reply& r, const char* key, double& v) {
    if (r.outcome != Outcome::Ok && r.outcome != Outcome::OkInferred) return false;
    for (auto& l : r.lines) {
      size_t at = l.find(key);
      if (at == std::string::npos) continue;
      char* end;
      double p = strtod(l.c_str() + at + strlen(key), &end);
      if (end != l.c_str() + at + strlen(key) && *end == '%' && p > 0) return v = p / 100, true;
    }
    return false;
  };
  printer_.send_raw("M220", [this, percent](const Reply& r) { percent(r, "FR:", speed_factor_); });
  bool sent = printer_.send_raw("M221", [this, percent](const Reply& r) {
    factors_query_out_ = false;
    percent(r, " Flow: ", extrude_factor_);
  });
  if (!sent) factors_query_out_ = false;
}

// ---- calibration -----------------------------------------------------------------------

bool Service::calibration_allowed(std::string& err) const {
  Job j = printer_.status().job;
  if (pstate_ == PrintState::Printing || pstate_ == PrintState::Paused || handoff_active_ ||
      j == Job::Printing || j == Job::Paused)
    return err = "not while printing", false;
  if (delivery_.busy()) return err = "Halyard is copying a file to the printer", false;
  if (pid_running_) return err = "a PID autotune is running", false;
  if (probe_.starting || probe_.active)
    return err = "CALIBRATE_Z_OFFSET is running: ACCEPT or ABORT it first", false;
  return true;
}

bool Service::pid_tune(const std::shared_ptr<Script>& s, const std::string& heater, double target,
                       std::string& err) {
  bool bed = heater == "heater_bed";
  if (!bed && heater != "extruder") return err = "The value '" + heater + "' is not valid for HEATER", false;
  double max = bed ? kMaxBed : kMaxHotend;
  if (!(target >= 40 && target <= max))
    return err = "TARGET must be between 40 and " + std::to_string((int)max), false;
  if (!calibration_allowed(err)) return false;
  // Eight cycles rather than Marlin's five, and U1: the result is used at once.
  char buf[48];
  snprintf(buf, sizeof buf, "M303 E%d S%d C8 U1", bed ? -1 : 0, (int)std::lround(target));
  respond(std::string("// PID autotune of the ") + (bed ? "bed" : "hotend") + " at " +
          std::to_string((int)std::lround(target)) + "C: this takes several minutes");
  pid_running_ = true;
  bool ok = script_send(s, buf, err, [this, heater](const Reply& r, std::string& e) {
    pid_running_ = false;
    if (!e.empty()) return;
    std::string failed = "PID autotune failed";
    for (auto& l : r.lines) {
      std::string t = console_text(l);
      if (t.find("PID Autotune finished") != std::string::npos) failed.clear();
      else if (t.find("PID Autotune failed") != std::string::npos || starts_with(t, "Unknown command"))
        failed = t;
    }
    if (!failed.empty()) {
      e = failed;
      return;
    }
    respond("// The new PID values are in use." + keep_until_saved(heater, "pid"));
  });
  if (!ok) pid_running_ = false;
  return ok;
}

// PROBE_CALIBRATE for Marlin, the way Marlin's own PROBE_OFFSET_WIZARD does it: probe the
// bed's centre with G30, put the nozzle over the same spot at the height the probe triggered
// at, and let a person lower it onto a sheet of paper. The new offset is the old one plus how
// far the nozzle went below the probe's measurement. Works whether Z homes with the probe or
// with an endstop.
bool Service::z_calibrate(const std::shared_ptr<Script>& s, std::string& err) {
  if (!has_probe()) return err = "the printer reports no Z probe (Cap:Z_PROBE in M115)", false;
  if (!calibration_allowed(err)) return false;
  probe_ = ManualProbe();
  probe_.starting = true;
  bool ok = script_send(s, "G28", err) &&
            script_send(s, "M211", err, [this](const Reply& r, std::string&) {
              for (auto& l : r.lines) probe_.soft_endstops |= l.find("M211 S1") != std::string::npos;
            });
  // G30 switches bed leveling off; it is switched on again afterwards if it was on.
  if (ok && link_.caps().has("LEVELING_DATA"))
    ok = script_send(s, "M420", err, [this](const Reply& r, std::string&) {
      for (auto& l : r.lines) probe_.leveling |= l.find("Bed Leveling ON") != std::string::npos;
    });
  ok = ok && script_send(s, "M851", err, [this](const Reply& r, std::string& e) {
    bool found = false;
    for (auto& l : r.lines) {
      size_t at = l.find("M851");  // "  M851 X-33.00 Y-15.00 Z-0.90 ; (mm)"
      found |= at != std::string::npos && number_after(l.substr(at), " Z", probe_.offset);
    }
    if (!found && e.empty()) e = "cannot read the probe's Z offset (M851)";
  });
  char g30[64];
  snprintf(g30, sizeof g30, "G30 X%.1f Y%.1f", (axis_min_[0] + axis_max_[0]) / 2,
           (axis_min_[1] + axis_max_[1]) / 2);
  std::string xy = std::string(g30).substr(4);
  ok = ok && script_send(s, g30, err, [this, s, xy](const Reply& r, std::string& e) {
    bool found = false;
    for (auto& l : r.lines) found |= starts_with(trim(l), "Bed X:") && number_after(l, " Z: ", probe_.ref);
    if (!found && e.empty()) e = "probing the bed's centre failed";
    if (!e.empty() || !s->error.empty()) {
      probe_.starting = false;
      z_restore(s);
      return;
    }
    // The nozzle where the probe measured, at the height the probe triggered at.
    probe_.z = probe_.ref - probe_.offset;
    std::string e2;
    bool sent = script_send(s, "M211 S0", e2) &&  // the nozzle may need to go below Z0
                script_send(s, "G90", e2) && script_send(s, "G1 Z10 F600", e2) &&
                script_send(s, "G1 " + xy + " F6000", e2) &&
                script_send(s, "G1 Z" + fmt3(probe_.z) + " F300", e2) &&
                script_send(s, "M400", e2, [this](const Reply&, std::string& e3) {
                  probe_.starting = false;
                  probe_.active = e3.empty();
                  if (probe_.active)
                    respond("// Lower the nozzle with TESTZ until it just grips a sheet of paper, "
                            "then ACCEPT (or ABORT)");
                });
    if (!sent) {
      e = e2;
      probe_.starting = false;
    }
  });
  if (!ok) probe_.starting = false;
  return ok;
}

bool Service::z_test(const std::shared_ptr<Script>& s, const std::string& arg, std::string& err) {
  if (!probe_.active) return err = "no manual probe is running: start CALIBRATE_Z_OFFSET first", false;
  // Klipper's manual_probe: + and - bisect towards the nearest position tried above or below,
  // ++ and -- go to it; either way at most kBisectMax at a time.
  std::vector<double>& t = probe_.tried;
  double z = probe_.z, next;
  auto at = std::lower_bound(t.begin(), t.end(), z);
  if (at == t.end() || *at != z) at = t.insert(at, z);
  if (arg == "+" || arg == "++") {
    double check = at + 1 != t.end() ? *(at + 1) : 1e9;
    if (arg == "+") check = (check + z) / 2;
    next = std::min(check, z + kBisectMax);
  } else if (arg == "-" || arg == "--") {
    double check = at != t.begin() ? *(at - 1) : -1e9;
    if (arg == "-") check = (check + z) / 2;
    next = std::max(check, z - kBisectMax);
  } else {
    char* end;
    double d = strtod(arg.c_str(), &end);
    if (end == arg.c_str() || *end) return err = "Z must be a number, +, -, ++ or --", false;
    if (std::fabs(d) > 5) return err = "Z must be within 5 mm", false;
    next = z + d;
  }
  next = std::round(next * 1000) / 1000;
  if (!script_send(s, "G90", err) || !script_send(s, "G1 Z" + fmt3(next) + " F300", err))
    return false;
  probe_.z = next;
  respond("// Z " + fmt3(next));
  return true;
}

bool Service::z_finish(const std::shared_ptr<Script>& s, bool accept, std::string& err) {
  if (!probe_.active) return err = "no manual probe is running: start CALIBRATE_Z_OFFSET first", false;
  probe_.active = false;
  double z = probe_.z;
  double offset = std::round((probe_.offset + z - probe_.ref) * 1000) / 1000;
  bool ok = script_send(s, "G90", err) && script_send(s, "G1 Z" + fmt3(z + 5) + " F600", err);
  if (ok && accept) {
    // Z's coordinates were made with the old offset when Z homes with the probe: home Z again.
    ok = script_send(s, "M851 Z" + fmt3(offset), err) &&
         script_send(s, "G28 Z", err, [this, offset](const Reply&, std::string& e) {
           if (!e.empty()) return;
           respond("// Probe Z offset " + fmt3(offset) + " (it was " + fmt3(probe_.offset) + ")." +
                   keep_until_saved("probe", "z_offset " + fmt3(offset)));
         });
  }
  z_restore(s);
  return ok;
}

void Service::z_restore(const std::shared_ptr<Script>& s) {
  std::string e;
  if (probe_.soft_endstops) script_send(s, "M211 S1", e);
  if (probe_.leveling) script_send(s, "M420 S1", e);
}

int Service::m_gcode_script(const Call& c, JsonVariantConst p, JsonDocument& r,
                            std::string& err) {
  std::string script = str(p["script"]);
  if (trim(script).empty()) return err = "missing script", kBadRequest;
  if (link_.state() != LinkState::Ready) {
    // Only Halyard's own restart works without the printer: SAVE & RESTART reads halyard.cfg.
    for (auto& raw : split(script, '\n')) {
      std::string line = trim(raw);
      if (line.empty()) continue;
      std::string cmd = upper(words_of(line)[0]);
      if (cmd != "FIRMWARE_RESTART" && cmd != "RESTART") return err = "the printer is not connected", kUnavailable;
    }
  }
  for (auto& raw : split(script, '\n'))
    if (!trim(raw).empty()) store_gcode(trim(raw), true);
  if (!moves_homed(script, err)) {
    respond("!! " + err);
    return kBadRequest;
  }
  auto s = std::make_shared<Script>();
  s->call = c;
  if (c.conn < 0) s->call.id.clear();  // over HTTP the reply goes now
  scripts_running_++;
  for (auto& raw : split(script, '\n')) {
    std::string line = trim(raw);
    if (line.empty()) continue;
    std::string e;
    if (!run_line(s, line, e)) {
      s->error = e;
      respond("!! " + e);
      break;
    }
  }
  s->all_sent = true;
  if (s->outstanding > 0) {
    if (c.conn >= 0 && !c.id.empty()) return kDeferred;
    r.set("ok");
    return 0;
  }
  scripts_running_--;
  if (!s->error.empty()) return err = s->error, kBadRequest;
  r.set("ok");
  return 0;
}

int Service::m_gcode_help(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  for (auto& c : kCommands)
    if (command_available(c[0])) r[c[0]] = c[1];
  return 0;
}

int Service::m_gcode_store(const Call&, JsonVariantConst p, JsonDocument& r, std::string&) {
  size_t count = (size_t)num(p["count"], kGcodeStoreMax);
  JsonArray a = r["gcode_store"].to<JsonArray>();
  size_t start = gcode_store_.size() > count ? gcode_store_.size() - count : 0;
  for (size_t i = start; i < gcode_store_.size(); i++) {
    JsonObject o = a.add<JsonObject>();
    o["message"] = gcode_store_[i].message;
    o["time"] = round2(gcode_store_[i].time);
    o["type"] = gcode_store_[i].command ? "command" : "response";
  }
  return 0;
}

int Service::m_temperature_store(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  // Up to 4 x 1200 numbers: written by hand, far smaller than a document of them.
  std::string out = "{";
  const char* sensors[] = {"extruder", "heater_bed"};
  char buf[16];
  for (int s = 0; s < 2; s++) {
    out += std::string(s ? "," : "") + "\"" + sensors[s] + "\":{";
    const char* series[] = {"temperatures", "targets", "powers"};
    for (int k = 0; k < 3; k++) {
      out += std::string(k ? "," : "") + "\"" + series[k] + "\":[";
      for (size_t i = 0; i < temp_count_; i++) {
        size_t idx = (temp_head_ + kTempSamples - temp_count_ + i) % kTempSamples;
        if (k == 2)
          snprintf(buf, sizeof buf, "%s%.2f", i ? "," : "", powers_[s][idx] / 100.0);
        else
          snprintf(buf, sizeof buf, "%s%.1f", i ? "," : "", temps_[s * 2 + k][idx]);
        out += buf;
      }
      out += "]";
    }
    out += "}";
  }
  out += "}";
  r.set(serialized(out));
  return 0;
}

// ---- server and machine ----------------------------------------------------------------

int Service::m_identify(const Call& c, JsonVariantConst, JsonDocument& r, std::string&) {
  if (c.conn >= 0 && !conns_.count(c.conn)) ws_open(c.conn);
  r["connection_id"] = c.conn;
  return 0;
}

int Service::m_server_info(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  r["klippy_connected"] = true;
  r["klippy_state"] = klippy_state();
  JsonArray comps = r["components"].to<JsonArray>();
  for (const char* c : {"server", "file_manager", "machine", "database", "data_store",
                        "proc_stats", "history", "octoprint_compat"})
    comps.add(c);
  // Mainsail shows its panel then.
  if (platform_.ui_version || !platform_.firmware.empty()) comps.add("update_manager");
  r["failed_components"].to<JsonArray>();
  JsonArray dirs = r["registered_directories"].to<JsonArray>();
  dirs.add("gcodes");
  dirs.add("config");
  JsonArray warnings = r["warnings"].to<JsonArray>();
  if (card_missing()) warnings.add(kNoCard);
  for (auto& w : config_warnings_) warnings.add(w);
  r["websocket_count"] = (int)conns_.size();
  r["moonraker_version"] = kMoonrakerVersion;
  JsonArray api = r["api_version"].to<JsonArray>();
  api.add(1);
  api.add(5);
  api.add(0);
  r["api_version_string"] = "1.5.0";
  r["type"] = "halyard";
  return 0;
}

int Service::m_server_config(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  JsonObject cfg = r["config"].to<JsonObject>();
  cfg["server"]["host"] = "0.0.0.0";
  cfg["server"]["port"] = 80;
  cfg["file_manager"]["queue_gcode_uploads"] = false;
  cfg["file_manager"]["enable_object_processing"] = false;
  cfg["database"].to<JsonObject>();
  cfg["history"].to<JsonObject>();
  cfg["octoprint_compat"].to<JsonObject>();
  r["orig"] = r["config"];
  r["files"].to<JsonArray>();
  return 0;
}

int Service::m_printer_info(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  std::string k = klippy_state();
  r["state"] = k;
  r["state_message"] = klippy_message(k);
  r["hostname"] = platform_.hostname;
  // Halyard's version alone: Mainsail puts app before it, and Marlin has its own row (the
  // mcu object). Halyard's Mainsail checks Klipper's minimum version only against Klipper.
  r["software_version"] = platform_.version;
  r["cpu_info"] = platform_.hardware;
  r["app"] = "Halyard";
  return 0;
}

int Service::m_system_info(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  JsonObject si = r["system_info"].to<JsonObject>();
  JsonObject cpu = si["cpu_info"].to<JsonObject>();
  cpu["cpu_count"] = 2;
  cpu["bits"] = "32bit";
  cpu["processor"] = "xtensa";
  cpu["cpu_desc"] = platform_.hardware;
  cpu["serial_number"] = "";
  cpu["hardware_desc"] = "";
  cpu["model"] = platform_.hardware;
  cpu["total_memory"] = platform_.free_memory ? platform_.free_memory() / 1024 : 0;
  cpu["memory_units"] = "kB";
  si["sd_info"].to<JsonObject>();
  JsonObject dist = si["distribution"].to<JsonObject>();
  // What Halyard runs on, where Moonraker names the Linux distribution (Mainsail's "OS").
  dist["name"] = platform_.os;
  dist["id"] = platform_.os.empty() ? "" : "esp-idf";
  dist["version"] = platform_.os;
  dist["like"] = "";
  dist["codename"] = "";
  si["network"].to<JsonObject>();
  si["available_services"].to<JsonArray>();
  si["service_state"].to<JsonObject>();
  return 0;
}

int Service::m_proc_stats(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  uint32_t free_kb = platform_.free_memory ? platform_.free_memory() / 1024 : 0;
  JsonObject ms = r["moonraker_stats"].to<JsonArray>().add<JsonObject>();
  ms["time"] = round2(wall());
  ms["cpu_usage"] = 0;
  ms["memory"] = 0;
  ms["mem_units"] = "kB";
  r["throttled_state"]["bits"] = 0;
  r["throttled_state"]["flags"].to<JsonArray>();
  if (cpu_temp_known_) r["cpu_temp"] = round2(cpu_temp_);
  else r["cpu_temp"] = nullptr;
  r["network"].to<JsonObject>();
  r["system_cpu_usage"]["cpu"] = 0;
  r["system_memory"]["available"] = free_kb;
  r["system_uptime"] = round2(uptime());
  r["websocket_connections"] = (int)conns_.size();
  return 0;
}

// machine.update.status (Mainsail asks with refresh: true for "check for updates"), and
// machine.update.refresh. The check runs in the background; its result comes as
// notify_update_refreshed.
int Service::m_update_status(const Call&, JsonVariantConst p, JsonDocument& r, std::string&) {
  if (truthy(p["refresh"]) && on_update_check && !update_busy_) on_update_check();
  update_status(r.to<JsonObject>());
  return 0;
}

int Service::m_update_refresh(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  if (on_update_check && !update_busy_) on_update_check();
  update_status(r.to<JsonObject>());
  return 0;
}

void Service::update_status(JsonObject r) {
  r["busy"] = update_busy_;
  r["github_rate_limit"] = 60;
  r["github_requests_remaining"] = 60;
  r["github_limit_reset_time"] = 0;
  JsonObject info = r["version_info"].to<JsonObject>();
  // A web client's row: Mainsail offers the update when remote_version is above version by
  // semver, and links to the release tagged remote_version in owner/repo_name.
  auto row = [&](const char* name, const std::string& version, const std::string& remote) {
    JsonObject m = info[name].to<JsonObject>();
    m["name"] = name;
    m["owner"] = "tallbiscuits";  // Halyard's build of Mainsail ships with Halyard's releases
    m["repo_name"] = "halyard";
    m["version"] = version.empty() ? "?" : version;
    m["remote_version"] = remote.empty() ? "?" : remote;
    m["channel"] = "stable";
    m["configured_type"] = "web";
    m["detected_type"] = "web";
    m["is_valid"] = true;
    m["info_tags"].to<JsonArray>();
    m["anomalies"].to<JsonArray>();
    return m["warnings"].to<JsonArray>();
  };
  if (platform_.ui_version) {
    JsonArray w = row("mainsail", platform_.ui_version(), platform_.ui_release);
    if (!ui_check_error_.empty()) w.add("Mainsail " + platform_.ui_release + " cannot be fetched: " + ui_check_error_);
  }
  if (!platform_.firmware.empty()) {
    // Tags are "v0.5.0"; the firmware names itself "0.5.0".
    JsonArray w = row("halyard", "v" + platform_.firmware, release_.empty() ? "" : "v" + release_);
    if (!release_error_.empty()) w.add("No Halyard release found: " + release_error_);
  }
}

void Service::update_checked(const std::string& ui_error, const std::string& release,
                             const std::string& release_error) {
  ui_check_error_ = ui_error;
  release_ = release;
  release_error_ = release_error;
  JsonDocument d;
  update_status(d.to<JsonArray>().add<JsonObject>());
  notify("notify_update_refreshed", d.as<JsonVariantConst>());
}

// Mainsail's update button (machine.update.client, or .upgrade in newer Moonraker).
int Service::m_update_client(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  return start_update(str(p["name"]), r, err);
}

// Its "update all", shown when both rows have an update: the firmware first, since a new
// firmware names a new Mainsail, which its own row then offers.
int Service::m_update_full(const Call&, JsonVariantConst, JsonDocument& r, std::string& err) {
  bool firmware = !platform_.firmware.empty() && !release_.empty() && release_ != platform_.firmware;
  return start_update(firmware ? "halyard" : "mainsail", r, err);
}

int Service::start_update(const std::string& name, JsonDocument& r, std::string& err) {
  bool known = (name == "mainsail" && platform_.ui_version) ||
               (name == "halyard" && !platform_.firmware.empty());
  if (!known || !on_update) return err = "Halyard cannot update " + name, kBadRequest;
  if (update_busy_) return err = "an update is running already", kConflict;
  if (name == "halyard") {
    if (release_.empty()) return err = "no Halyard release found yet: check for updates first", kBadRequest;
    if (!can_restart(err)) return kConflict;  // the new firmware starts with a restart
  }
  if (!on_update(name, err)) return kBadRequest;
  update_busy_ = true;
  r.set("ok");
  return 0;
}

void Service::update_progress(const std::string& name, const std::string& message, bool complete) {
  if (complete) update_busy_ = false;
  JsonDocument d;
  JsonObject o = d.to<JsonArray>().add<JsonObject>();
  o["application"] = name;
  o["proc_id"] = 1;
  o["message"] = ascii(message);
  o["complete"] = complete;
  notify("notify_update_response", d.as<JsonVariantConst>());
}

int Service::m_machine_reboot(const Call&, JsonVariantConst, JsonDocument& r, std::string& err) {
  if (!platform_.reboot) return err = "cannot reboot", kBadRequest;
  if (delivery_.busy()) delivery_.abort();
  reboot_soon();
  r.set("ok");
  return 0;
}

void Service::reboot_soon() {
  reboot_at_ = now_ + 500;
  if (!reboot_at_) reboot_at_ = 1;
}

// A print survives Halyard restarting (the printer feeds itself), a copy to the printer
// does not.
bool Service::can_restart(std::string& why) const {
  if (!delivery_.busy()) return true;
  why = "a file is being copied to the printer; wait, or cancel it";
  return false;
}

int Service::m_webcams(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  r["webcams"].to<JsonArray>();  // must succeed, or Mainsail hangs at "Initializing"
  return 0;
}

int Service::m_announcements(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  r["entries"].to<JsonArray>();
  r["feeds"].to<JsonArray>();
  return 0;
}

// ---- objects ---------------------------------------------------------------------------

int Service::m_objects_list(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  JsonArray a = r["objects"].to<JsonArray>();
  for (auto& n : object_names()) a.add(n);
  return 0;
}

int Service::m_objects_query(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  if (!p["objects"].is<JsonObjectConst>()) return err = "no objects", kBadRequest;
  r["eventtime"] = round2(uptime());
  objects_status(p["objects"], r["status"].to<JsonObject>(), nullptr);
  return 0;
}

int Service::m_objects_subscribe(const Call& c, JsonVariantConst p, JsonDocument& r,
                                 std::string& err) {
  int conn = c.conn >= 0 ? c.conn : (int)num(p["connection_id"], -1);
  auto it = conns_.find(conn);
  if (it == conns_.end()) return err = "no such websocket connection", kBadRequest;
  r["eventtime"] = round2(uptime());
  objects_status(p["objects"], r["status"].to<JsonObject>(), &it->second);
  return 0;
}

// ---- printing --------------------------------------------------------------------------

int Service::m_print_start(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string name = str(p["filename"]);
  std::string stripped;
  if (strip_gcodes_root(name, stripped)) name = stripped;
  if (!Library::valid_name(name)) return err = "not a file in Halyard's library: " + name, kBadRequest;
  if (!hand_off(name, true, err)) return kBadRequest;
  r.set("ok");
  return 0;
}

int Service::m_print_pause(const Call& c, JsonVariantConst, JsonDocument& r, std::string& err) {
  if (pstate_ != PrintState::Printing || handoff_active_) return err = "not printing", kBadRequest;
  Call call = c;
  printer_.pause([this, call](bool ok, const std::string& msg) {
    JsonDocument d;
    d.set("ok");
    if (!ok) respond("!! pause: " + msg);
    reply(call, ok ? 0 : kBadRequest, d, msg);
  });
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  r.set("ok");
  return 0;
}

int Service::m_print_resume(const Call& c, JsonVariantConst, JsonDocument& r, std::string& err) {
  if (pstate_ != PrintState::Paused) return err = "not paused", kBadRequest;
  Call call = c;
  printer_.resume([this, call](bool ok, const std::string& msg) {
    JsonDocument d;
    d.set("ok");
    if (!ok) respond("!! resume: " + msg);
    reply(call, ok ? 0 : kBadRequest, d, msg);
  });
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  r.set("ok");
  return 0;
}

int Service::m_print_cancel(const Call& c, JsonVariantConst, JsonDocument& r, std::string& err) {
  // During the copy nothing prints yet: stopping the hand-off switches the heaters off. A
  // copy without a print is stopped the same way (Halyard's Mainsail offers it).
  if (handoff_active_) {
    cancel_requested_ = true;
    delivery_.abort();
    r.set("ok");
    return 0;
  }
  if (pstate_ != PrintState::Printing && pstate_ != PrintState::Paused)
    return err = "not printing", kBadRequest;
  Call call = c;
  // The printer may home X/Y as it cancels (EVENT_GCODE_SD_ABORT); done once it is idle.
  printer_.cancel([this, call](bool ok, const std::string& msg) {
    JsonDocument d;
    d.set("ok");
    // Marlin's abort homes X/Y and cools down, but leaves the steppers on; only a finished
    // print releases them (SD_FINISHED_RELEASECOMMAND). M84 waits for the homing to end.
    if (ok) printer_.send_raw("M84", nullptr);
    if (!ok) respond("!! cancel: " + msg);
    reply(call, ok ? 0 : kBadRequest, d, msg);
  });
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  r.set("ok");
  return 0;
}

int Service::m_emergency_stop(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  if (delivery_.busy()) {
    // A hand-off never runs during a print, so nothing moves: stop it, heaters off.
    cancel_requested_ = true;
    delivery_.abort();
    respond("!! Emergency stop: hand-off stopped, heaters off");
  } else if (link_.caps().has("EMERGENCY_PARSER")) {
    link_.send_now("M112");
  } else {
    link_.send("M112");
  }
  r.set("ok");
  return 0;
}

// Mainsail's restart buttons do what the G-code does: read halyard.cfg again and reconnect.
int Service::m_restart(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  if (delivery_.busy()) delivery_.abort();
  if (on_restart) on_restart();
  link_.reconnect();
  r.set("ok");
  return 0;
}

// Mainsail's Endstops panel (Machine page) as M119, with Marlin's names (x_min, z_probe)
// where Klipper gives its steppers' (x, z). Only endstops and the probe: M119 also reports a
// filament sensor and probe_en, which are not. With a BLTouch, Marlin reads it in switch
// mode. During a print the answer waits behind the job's queued lines, as every reply does.
int Service::m_query_endstops(const Call& c, JsonVariantConst, JsonDocument& r, std::string& err) {
  if (link_.state() != LinkState::Ready) return err = "the printer is not connected", kUnavailable;
  Call call = c;
  bool sent = printer_.send_raw("M119", [this, call](const Reply& rep) {
    JsonDocument d;
    if (rep.outcome != Outcome::Ok && rep.outcome != Outcome::OkInferred) {
      reply(call, kUnavailable, d, "the printer did not answer M119");
      return;
    }
    endstops_.clear();
    for (auto& l : rep.lines) {
      std::string t = trim(l);
      size_t colon = t.find(": ");
      if (colon == std::string::npos) continue;
      std::string name = t.substr(0, colon), state = t.substr(colon + 2);
      std::string end = name.size() > 4 ? name.substr(name.size() - 4) : "";
      bool endstop = name == "z_probe" || end == "_min" || end == "_max";
      if (endstop && (state == "open" || state == "TRIGGERED")) endstops_[name] = state;
    }
    JsonObject o = d.to<JsonObject>();
    for (auto& kv : endstops_) o[kv.first] = kv.second;
    reply(call, 0, d, "");
  });
  if (!sent) return err = "the printer is busy receiving a file", kUnavailable;
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  JsonObject o = r.to<JsonObject>();
  for (auto& kv : endstops_) o[kv.first] = kv.second;
  return 0;
}

// ---- files -----------------------------------------------------------------------------

bool Service::file_info(const std::string& name, JsonObject o, bool with_path) {
  struct stat st;
  if (stat(lib_.path(name).c_str(), &st) != 0) return false;
  o[with_path ? "path" : "filename"] = name;
  o["modified"] = (double)st.st_mtime;
  o["size"] = (uint32_t)st.st_size;
  o["permissions"] = "rw";
  return true;
}

bool Service::download_text(const std::string& path, std::string& body) const {
  std::string p = url_decode(path);
  if (p == std::string("config/") + kMarlinName && !settings_report_.empty() &&
      !platform_.config_path.empty()) {
    body = marlin_text();
    return true;
  }
  if (p != std::string("config/") + kConfigName || platform_.config_path.empty())
    return false;
  std::string text;
  if (!read_file(platform_.config_path, text)) return false;
  body = mask_password(text);
  return true;
}

bool Service::config_info(JsonObject o, bool with_path) {
  struct stat st;
  if (platform_.config_path.empty() || stat(platform_.config_path.c_str(), &st) != 0) return false;
  o[with_path ? "path" : "filename"] = kConfigName;
  o["modified"] = (double)st.st_mtime;
  o["size"] = (uint32_t)st.st_size;
  o["permissions"] = "rw";
  return true;
}

// ---- marlin.cfg: Marlin's settings, as M503 reports them ---------------------------------

void Service::ask_settings() {
  settings_stale_ = false;
  settings_query_out_ = printer_.send_raw("M503", [this](const Reply& r) {
    settings_query_out_ = false;
    // Disconnected: read again once it reconnects. Lost or Error: perhaps only part of the
    // report; keep the last one until the next change, print or connect asks again.
    if (r.outcome != Outcome::Ok && r.outcome != Outcome::OkInferred) return;
    // During a print the job's own lines may come in between: keep M503's. Without it
    // (DISABLE_M503) the answer is "Unknown command", and there is no marlin.cfg.
    std::string report;
    std::set<std::string> codes;
    for (auto& raw : r.lines) {
      std::string l = raw;
      while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
      if (!is_ascii(l)) continue;
      if (starts_with(l, "echo:")) {
        l = l.substr(5);
        if (!starts_with(l, ";") && !starts_with(l, " ")) continue;  // busy:, Unknown command
      } else if (!starts_with(l, "  ")) {
        continue;
      }
      report += l + "\n";
      std::string c = code_of(l);  // none for a heading
      if (!c.empty()) codes.insert(c);
    }
    bool changed = report != settings_report_;
    bool was_listed = !settings_report_.empty();
    settings_report_ = report;
    settings_codes_ = codes;
    settings_time_ = wall();
    if (report.empty() && !was_listed) return;
    JsonDocument d;
    JsonObject p = d.to<JsonArray>().add<JsonObject>();
    JsonObject item = p["item"].to<JsonObject>();
    if (report.empty()) {
      p["action"] = "delete_file";
      item["path"] = kMarlinName;
    } else {
      if (!changed && was_listed) return;
      p["action"] = was_listed ? "modify_file" : "create_file";
      marlin_info(item, true);
    }
    item["root"] = "config";
    notify("notify_filelist_changed", d.as<JsonVariantConst>());
  });
  if (!settings_query_out_) settings_stale_ = true;  // the link was busy: next time
}

bool Service::marlin_info(JsonObject o, bool with_path) const {
  if (settings_report_.empty() || platform_.config_path.empty()) return false;
  o[with_path ? "path" : "filename"] = kMarlinName;
  o["modified"] = round2(settings_time_ ? settings_time_ : wall());
  o["size"] = (uint32_t)marlin_text().size();
  o["permissions"] = "r";  // Mainsail's editor opens it read-only, and offers no delete
  return true;
}

std::string Service::marlin_text() const {
  std::string when;
  if (settings_time_ > 0) {
    time_t t = (time_t)settings_time_;
    struct tm tm;
    char buf[40];
    gmtime_r(&t, &tm);
    strftime(buf, sizeof buf, " on %Y-%m-%d at %H:%M UTC", &tm);
    when = buf;
  }
  std::string keep = link_.caps().has("EEPROM")
                         ? "#   SAVE CONFIG (M500) keeps it in the printer's EEPROM.\n"
                         : "#   This printer has no EEPROM (EEPROM_SETTINGS), so a change lasts until it restarts.\n";
  return "# Marlin's settings, as the printer reported them (M503)" + when + ".\n"
         "# Read-only. To change a setting, send its command on the console (M92 E93.5, say).\n" +
         keep +
         "#   Halyard reads this again after such a change, after a print and on connecting.\n\n" +
         settings_report_;
}

// Saved as given: a mistake in it shows up when it is read (SAVE & RESTART), as a warning.
bool Service::save_config(const std::string& text, std::string& err) {
  std::string current;
  if (!read_file(platform_.config_path, current)) current.clear();
  if (!write_file(platform_.config_path, unmask_password(text, current)))
    return err = "cannot write halyard.cfg", false;
  JsonDocument d;
  JsonObject p = d.to<JsonArray>().add<JsonObject>();
  p["action"] = "create_file";
  JsonObject item = p["item"].to<JsonObject>();
  config_info(item, true);
  item["root"] = "config";
  notify("notify_filelist_changed", d.as<JsonVariantConst>());
  return true;
}

void Service::set_config_warnings(const std::vector<std::string>& w) {
  config_warnings_ = w;
  for (auto& line : w) respond("!! " + line);
}

void Service::show_on_printer(const std::string& text) {
  // The line goes out with a line number and a checksum: no '*', and no ';' (a comment).
  std::string t;
  for (unsigned char c : text.substr(0, 40)) t += c >= 0x20 && c < 0x7f && c != '*' && c != ';' ? (char)c : ' ';
  screen_text_ = t;
}

void Service::screen_poll() {
  if (link_.state() != LinkState::Ready) {
    screen_shown_.clear();  // a reconnected printer has a fresh screen
    return;
  }
  if (screen_text_.empty() || screen_text_ == screen_shown_) return;
  // Not over a print's own message, and not into a file being written.
  if (printer_.status().job != Job::Idle || delivery_.busy() || printer_.uploading()) return;
  if (printer_.send_raw("M117 " + screen_text_, nullptr)) screen_shown_ = screen_text_;
}

void Service::file_changed(const char* action, const std::string& name, const std::string& source) {
  JsonDocument d;
  JsonObject p = d.to<JsonArray>().add<JsonObject>();
  p["action"] = action;
  JsonObject item = p["item"].to<JsonObject>();
  if (!file_info(name, item, true)) item["path"] = name;
  item["root"] = "gcodes";
  if (!source.empty()) {
    p["source_item"]["path"] = source;
    p["source_item"]["root"] = "gcodes";
  }
  notify("notify_filelist_changed", d.as<JsonVariantConst>());
}

void Service::metadata_json(const Metadata& m, JsonObject o) {
  if (!m.slicer.empty()) o["slicer"] = m.slicer;
  if (!m.slicer_version.empty()) o["slicer_version"] = m.slicer_version;
  auto put = [&](const char* k, double v) {
    if (v >= 0) o[k] = v;
  };
  put("estimated_time", m.estimated_time);
  put("filament_total", m.filament_total);
  put("filament_weight_total", m.filament_weight_total);
  put("layer_height", m.layer_height);
  put("first_layer_height", m.first_layer_height);
  put("object_height", m.object_height);
  put("first_layer_extr_temp", m.first_layer_extr_temp);
  put("first_layer_bed_temp", m.first_layer_bed_temp);
  if (m.layer_count >= 0) o["layer_count"] = m.layer_count;
  if (!m.thumbnails.empty()) {
    JsonArray a = o["thumbnails"].to<JsonArray>();
    for (auto& t : m.thumbnails) {
      JsonObject th = a.add<JsonObject>();
      th["width"] = t.width;
      th["height"] = t.height;
      th["size"] = t.size;
      th["relative_path"] = t.relative_path;
    }
  }
}

int Service::m_files_list(const Call&, JsonVariantConst p, JsonDocument& r, std::string&) {
  JsonArray a = r.to<JsonArray>();
  std::string root = p["root"].isNull() ? "gcodes" : str(p["root"]);
  if (root == "config") {
    JsonObject o = a.add<JsonObject>();
    if (!config_info(o, true)) a.remove(0);
    JsonObject m = a.add<JsonObject>();
    if (!marlin_info(m, true)) a.remove(a.size() - 1);
    return 0;
  }
  if (root != "gcodes") return 0;
  for (auto& n : lib_.names()) {
    JsonObject o = a.add<JsonObject>();
    if (!file_info(n, o, true)) a.remove(a.size() - 1);
  }
  return 0;
}

int Service::m_files_directory(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string path = p["path"].isNull() ? "gcodes" : str(p["path"]);
  while (!path.empty() && path.back() == '/') path.pop_back();
  while (!path.empty() && path[0] == '/') path.erase(0, 1);
  if (path != "gcodes" && path != "config")
    return err = "Directory does not exist", kNotFound;
  JsonArray dirs = r["dirs"].to<JsonArray>();
  JsonArray files = r["files"].to<JsonArray>();
  if (path == "gcodes") {
    bool extended = truthy(p["extended"]);
    for (auto& n : lib_.names()) {
      JsonObject o = files.add<JsonObject>();
      if (!file_info(n, o, false)) {
        files.remove(files.size() - 1);
        continue;
      }
      LibraryEntry e;
      if (extended && lib_.get(n, e)) metadata_json(e.meta, o);
    }
  } else if (path == "config") {
    JsonObject o = files.add<JsonObject>();
    if (!config_info(o, false)) files.remove(0);
    JsonObject m = files.add<JsonObject>();
    if (!marlin_info(m, false)) files.remove(files.size() - 1);
  }
  uint64_t total = 0, used = 0;
  if (platform_.disk && platform_.disk(total, used)) {
    r["disk_usage"]["total"] = total;
    r["disk_usage"]["used"] = used;
    r["disk_usage"]["free"] = total - used;
  } else {
    r["disk_usage"]["total"] = 0;
    r["disk_usage"]["used"] = 0;
    r["disk_usage"]["free"] = 0;
  }
  r["root_info"]["name"] = path.substr(0, path.find('/'));
  r["root_info"]["permissions"] = "rw";
  return 0;
}

int Service::m_files_metadata(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string name = str(p["filename"]);
  LibraryEntry e;
  if (!lib_.get(name, e)) return err = "Metadata not available for " + name, kNotFound;
  JsonObject o = r.to<JsonObject>();
  file_info(name, o, false);
  o["print_start_time"] = nullptr;
  o["job_id"] = nullptr;
  metadata_json(e.meta, o);
  return 0;
}

int Service::m_files_delete(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string name;
  if (!strip_gcodes_root(str(p["path"]), name)) return err = "only gcodes files can be deleted", kBadRequest;
  if (delivery_.busy() && delivery_.progress().name == name)
    return err = name + " is being copied to the printer", kConflict;
  if (uploading_.count(name)) return err = name + " is being uploaded", kConflict;
  JsonObject item = r["item"].to<JsonObject>();
  if (!file_info(name, item, true)) return err = "File does not exist: " + name, kNotFound;
  item["root"] = "gcodes";
  if (!lib_.remove(name)) return err = "cannot delete " + name, kBadRequest;
  r["action"] = "delete_file";
  JsonDocument d;
  d.add(r.as<JsonVariantConst>());
  notify("notify_filelist_changed", d.as<JsonVariantConst>());
  return 0;
}

// Moonraker writes the archive to its destination and Mainsail then downloads it. Here only
// the request is kept, and the archive is made while it downloads (zip_download), so a few
// GB of G-code need no room on the card and no time to copy.
int Service::m_files_zip(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::vector<std::string> items;
  uint64_t total = 0;
  for (JsonVariantConst v : p["items"].as<JsonArrayConst>()) {
    std::string item = str(v), name;
    while (!item.empty() && item[0] == '/') item.erase(0, 1);
    struct stat st;
    if (item == std::string("config/") + kConfigName && !platform_.config_path.empty() &&
        stat(platform_.config_path.c_str(), &st) == 0) {
    } else if (item == std::string("config/") + kMarlinName && !settings_report_.empty() &&
               !platform_.config_path.empty()) {
      st.st_size = (off_t)marlin_text().size();
    } else if (strip_gcodes_root(item, name) && Library::valid_name(name) &&
               stat((lib_.root() + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
    } else {
      return err = "File does not exist: " + item, kNotFound;
    }
    if (std::find(items.begin(), items.end(), item) != items.end()) continue;
    items.push_back(item);
    total += (uint64_t)st.st_size + 2 * item.size() + 100;
  }
  if (items.empty()) return err = "no files to zip", kBadRequest;
  if (total > 0xFFFF0000ull) return err = "more than 4 GB: download fewer files at once", kBadRequest;
  std::string dest = p["dest"].is<const char*>() ? str(p["dest"]) : "";
  while (!dest.empty() && dest[0] == '/') dest.erase(0, 1);
  if (dest.empty())
    dest = "config/collection-" + std::to_string((long long)(platform_.wall_time ? platform_.wall_time() : 0)) + ".zip";
  size_t slash = dest.find('/');
  std::string root = dest.substr(0, slash == std::string::npos ? 0 : slash);
  std::string file = slash == std::string::npos ? "" : dest.substr(slash + 1);
  if ((root != "config" && root != "gcodes") || file.empty() || file[0] == '.' ||
      file.find('/') != std::string::npos || file.find('\\') != std::string::npos)
    return err = "not a destination for a zip: " + dest, kBadRequest;
  zip_path_ = dest;
  zip_items_ = std::move(items);
  JsonObject d = r["destination"].to<JsonObject>();
  d["root"] = root;
  d["path"] = file;
  r["action"] = "zip_files";
  return 0;
}

std::unique_ptr<ZipStream> Service::zip_download(const std::string& path) const {
  if (zip_path_.empty() || url_decode(path) != zip_path_) return nullptr;
  std::unique_ptr<ZipStream> zip(new ZipStream);
  for (const std::string& item : zip_items_) {
    std::string name, text;
    if (strip_gcodes_root(item, name)) {
      zip->add_file(name, lib_.root() + "/" + name);
    } else if (item == std::string("config/") + kMarlinName && download_text(item, text)) {
      zip->add_text(kMarlinName, text, (time_t)settings_time_);
    } else if (download_text(item, text)) {
      struct stat st;
      zip->add_text(kConfigName, text,
                    stat(platform_.config_path.c_str(), &st) == 0 ? st.st_mtime : 0);
    }
  }
  return zip;
}

int Service::m_files_move(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string from, to;
  if (!strip_gcodes_root(str(p["source"]), from) || !strip_gcodes_root(str(p["dest"]), to))
    return err = "files stay in the gcodes root", kBadRequest;
  if (to.find('/') != std::string::npos) return err = "Halyard's library has no folders", kBadRequest;
  if ((delivery_.busy() && delivery_.progress().name == from) || uploading_.count(from) ||
      uploading_.count(to))
    return err = from + " is in use", kConflict;
  if (!lib_.rename(from, to, err)) return kBadRequest;
  file_info(to, r["item"].to<JsonObject>(), true);
  r["item"]["root"] = "gcodes";
  r["source_item"]["path"] = from;
  r["source_item"]["root"] = "gcodes";
  r["action"] = "move_file";
  file_changed("move_file", to, from);
  return 0;
}

int Service::m_files_no_folders(const Call&, JsonVariantConst, JsonDocument&, std::string& err) {
  err = "Halyard's library has no folders, and files are not copied";
  return kBadRequest;
}

// ---- the printer's card ----------------------------------------------------------------
//
// Halyard's Mainsail shows it in a panel of its own: what M20 lists, with the library file
// each one was copied from. Printing one starts it as it is (M23, M24); nothing is copied.

namespace {

// M20 T's "0x<date><time>": FAT's date and time, in the printer's local time. "" if absent,
// or if it is Marlin's FAT_DEFAULT_DATE and _TIME (2000-01-01 01:00): the printer has no
// clock, so every file it writes itself (each hand-off) gets that.
std::string fat_time(const std::string& hex) {
  if (hex.size() != 10 || hex.compare(0, 2, "0x") != 0) return "";
  unsigned long v = strtoul(hex.c_str() + 2, nullptr, 16);
  unsigned date = v >> 16, time = v & 0xFFFF;
  if (!date || (date == ((20u << 9) | (1u << 5) | 1u) && time == (1u << 11))) return "";
  char buf[24];
  snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", 1980 + (date >> 9), (date >> 5) & 15,
           date & 31, (time >> 11) & 31, (time >> 5) & 63);
  return buf;
}

}  // namespace

// Marlin up to 2.1.2.x copies each folder name of an M23 or M30 path with strncpy and no
// terminator (diveToFile()), so it opens the name plus whatever follows it on the stack and a
// file in a folder can't be printed or deleted over serial: "open failed". bugfix-2.1.x uses
// strlcpy. Found on the test printer, read from Marlin's source. Firmware that doesn't say
// which Marlin it is counts as having the bug.
bool Service::card_reachable(const std::string& name, std::string& why) const {
  if (name.find('/') == std::string::npos) return true;
  const std::string& fw = link_.caps().firmware;
  if (fw.compare(0, 13, "Marlin bugfix") == 0) return true;
  int a = 0, b = 0, c = 0;
  if (fw.compare(0, 7, "Marlin ") == 0 && sscanf(fw.c_str() + 7, "%d.%d.%d", &a, &b, &c) >= 2 &&
      (a > 2 || (a == 2 && (b > 1 || (b == 1 && c > 2)))))
    return true;
  why = "this Marlin can't open a file in a folder over serial (fixed after 2.1.2); "
        "use the printer's own menu, or a card reader";
  return false;
}

bool Service::card_ready(std::string& why) const {
  if (link_.state() != LinkState::Ready) return why = "the printer is not connected", false;
  Job j = printer_.status().job;
  if (j == Job::Unknown) return why = "the printer's state is not known yet", false;
  if (j != Job::Idle) return why = "the printer is printing", false;
  if (delivery_.busy() || printer_.uploading())
    return why = "a file is being copied to the printer", false;
  if (pid_running_ || probe_.starting || probe_.active)
    return why = "a calibration is running", false;
  return true;
}

void Service::card_list() {
  card_list_out_ = true;
  card_stale_ = false;
  printer_.list_files([this](bool ok, const std::vector<FileEntry>& files) {
    card_list_out_ = false;
    if (ok) {
      card_files_ = files;
      card_listed_ = true;
      card_listed_at_ = wall();
      card_error_.clear();
    } else {
      // Not tried again by itself: with no card in the printer it would never end.
      card_error_ = "the printer's card could not be listed; is there a card in the printer?";
    }
    card_version_++;
    std::vector<Call> waiting;
    waiting.swap(card_waiting_);
    for (auto& c : waiting) {
      JsonDocument d;
      card_json(d.to<JsonObject>());
      reply(c, 0, d, "");
    }
  });
}

const FileEntry* Service::card_file(const std::string& name) const {
  for (auto& f : card_files_)
    if (strcasecmp(f.name.c_str(), name.c_str()) == 0) return &f;
  return nullptr;
}

void Service::card_json(JsonObject r) const {
  // Which library file each 8.3 name belongs to, with the size it had when Halyard wrote it.
  std::map<std::string, std::pair<std::string, LibraryEntry>> mine;
  for (auto& n : lib_.names()) {
    LibraryEntry e;
    if (lib_.get(n, e) && !e.printer_name.empty()) mine[upper(e.printer_name)] = {n, e};
  }
  const Status& st = printer_.status();
  bool active = st.job == Job::Printing || st.job == Job::Paused;
  std::string printing = upper(st.file.substr(0, st.file.find(' ')));
  JsonArray a = r["files"].to<JsonArray>();
  for (auto& f : card_files_) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = ascii(f.name);
    o["size"] = f.size;
    if (!f.long_name.empty() && upper(f.long_name) != upper(f.name))
      o["long_name"] = ascii(f.long_name);
    std::string t = fat_time(f.timestamp);
    if (!t.empty()) o["modified"] = t;
    auto it = mine.find(upper(f.name));
    if (it != mine.end()) {
      const LibraryEntry& e = it->second.second;
      o["library"] = it->second.first;
      // False when the card's copy is not the one Halyard wrote (an interrupted copy).
      bool current = e.printer_size == f.size;
      o["current"] = current;
      // The printer can't send a file back, so this is all that is known of its contents.
      if (current) metadata_json(e.meta, o["metadata"].to<JsonObject>());
    }
    o["printing"] = active && upper(f.name) == printing;
    std::string why;
    if (!card_reachable(f.name, why)) o["locked"] = why;
  }
  r["listed"] = card_listed_;
  if (card_listed_) r["listed_at"] = round2(card_listed_at_);
  r["version"] = card_version_;
  if (!card_error_.empty()) r["error"] = card_error_;
}

// Lists the card again unless refresh is false. While the printer can't be asked (printing),
// the last listing comes back with "stale" saying why, refresh or not.
int Service::m_card_list(const Call& c, JsonVariantConst p, JsonDocument& r, std::string&) {
  bool refresh = p["refresh"].isNull() || truthy(p["refresh"]);
  std::string why;
  bool ready = card_ready(why);
  if (refresh && ready) {
    bool wait = c.conn >= 0 && !c.id.empty();
    if (wait) card_waiting_.push_back(c);
    if (!card_list_out_) card_list();
    if (wait) return kDeferred;
  }
  JsonObject o = r.to<JsonObject>();
  card_json(o);
  if (!ready) o["stale"] = why;
  return 0;
}

int Service::m_card_print(const Call& c, JsonVariantConst p, JsonDocument& r, std::string& err) {
  const FileEntry* f = card_file(str(p["name"]));
  if (!f) return err = "not on the printer's card: " + str(p["name"]), kNotFound;
  if (!card_reachable(f->name, err) || !card_ready(err)) return respond("!! Not printed: " + err), kConflict;
  std::string name = f->name;
  // The library's name when the card holds the copy Halyard wrote, so Mainsail shows its
  // thumbnail and metadata; otherwise the printer's own name for it.
  std::string job = f->long_name.empty() ? name : f->long_name;
  for (auto& n : lib_.names()) {
    LibraryEntry e;
    if (lib_.get(n, e) && strcasecmp(e.printer_name.c_str(), name.c_str()) == 0 &&
        e.printer_size == f->size)
      job = n;
  }
  Call call = c;
  respond("// Printing " + name + " from the printer's card");
  printer_.start_print(name, [this, call, name, job](bool ok, const std::string& msg) {
    JsonDocument d;
    d.set("ok");
    if (ok && pstate_ != PrintState::Printing) begin_job(ascii(job));
    if (!ok) respond("!! Print of " + name + " failed: " + msg);
    reply(call, ok ? 0 : kBadRequest, d, msg);
  });
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  r.set("ok");
  return 0;
}

int Service::m_card_delete(const Call& c, JsonVariantConst p, JsonDocument& r, std::string& err) {
  const FileEntry* f = card_file(str(p["name"]));
  if (!f) return err = "not on the printer's card: " + str(p["name"]), kNotFound;
  if (!card_reachable(f->name, err) || !card_ready(err)) return respond("!! Not deleted: " + err), kConflict;
  std::string name = f->name;
  Call call = c;
  bool sent = printer_.remove(name, [this, call, name](bool ok, const std::string& msg) {
    JsonDocument d;
    d.set("ok");
    if (ok) {
      respond("// Deleted " + name + " from the printer's card");
      card_files_.erase(std::remove_if(card_files_.begin(), card_files_.end(),
                                       [&](const FileEntry& e) { return e.name == name; }),
                        card_files_.end());
      card_version_++;
    } else {
      respond("!! Could not delete " + name + " from the printer's card: " + msg);
    }
    card_stale_ = true;  // what the card holds now, from the printer
    reply(call, ok ? 0 : kBadRequest, d, msg);
  });
  if (!sent) return err = "the printer is busy", kConflict;
  if (c.conn >= 0 && !c.id.empty()) return kDeferred;
  r.set("ok");
  return 0;
}

// A library file onto the printer's card, without printing it: a hand-off, as the console's
// send. Mainsail shows its progress through the halyard object; printer.print.cancel stops it.
// Refusals here and above also go to the console as "!!" lines, which Mainsail shows as a
// message on its Files page (a JSON-RPC error reaches only the browser's log).
int Service::m_card_copy(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string name = str(p["filename"]);
  std::string stripped;
  if (strip_gcodes_root(name, stripped)) name = stripped;
  if (!Library::valid_name(name)) return err = "not a file in Halyard's library: " + name, kBadRequest;
  if (!hand_off(name, false, err)) return respond("!! Not copied: " + err), kConflict;
  r.set("ok");
  return 0;
}

// ---- history ---------------------------------------------------------------------------

int Service::m_history_list(const Call&, JsonVariantConst p, JsonDocument& r, std::string&) {
  size_t start = (size_t)num(p["start"], 0), limit = (size_t)num(p["limit"], 50);
  JsonArray jobs = r["jobs"].to<JsonArray>();
  for (size_t i = start; i < history_.size() && jobs.size() < limit; i++)
    history_json(history_[i], jobs.add<JsonObject>());
  r["count"] = jobs.size();
  return 0;
}

int Service::m_history_totals(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  JsonObject t = r["job_totals"].to<JsonObject>();
  t["total_jobs"] = totals_.jobs;
  t["total_time"] = round2(totals_.time);
  t["total_print_time"] = round2(totals_.print_time);
  t["total_filament_used"] = round2(totals_.filament);
  t["longest_job"] = round2(totals_.longest_job);
  t["longest_print"] = round2(totals_.longest_print);
  r["auxiliary_totals"].to<JsonArray>();
  return 0;
}

// ---- Mainsail's database ---------------------------------------------------------------

int Service::m_db_list(const Call&, JsonVariantConst, JsonDocument& r, std::string&) {
  JsonArray a = r["namespaces"].to<JsonArray>();
  for (JsonPairConst kv : db_.as<JsonObjectConst>()) a.add(kv.key());
  r["backups"].to<JsonArray>();
  return 0;
}

static std::string db_key(JsonVariantConst k) { return k.isNull() ? "" : str(k); }

int Service::m_db_get(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string ns = str(p["namespace"]), key = db_key(p["key"]);
  if (ns.empty()) return err = "missing namespace", kBadRequest;
  JsonVariantConst v = db_.as<JsonObjectConst>()[ns];
  if (v.isNull()) return err = "Namespace '" + ns + "' not found", kNotFound;
  if (!key.empty()) {
    for (auto& part : split(key, '.')) {
      if (!v.is<JsonObjectConst>()) return err = "Key '" + key + "' in namespace '" + ns + "' not found", kNotFound;
      v = v[part];
    }
    if (v.isNull()) return err = "Key '" + key + "' in namespace '" + ns + "' not found", kNotFound;
  }
  r["namespace"] = ns;
  if (key.empty()) r["key"] = nullptr;
  else r["key"] = key;
  r["value"] = v;
  return 0;
}

int Service::m_db_post(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string ns = str(p["namespace"]), key = db_key(p["key"]);
  if (ns.empty() || key.empty()) return err = "missing namespace or key", kBadRequest;
  if (!db_.is<JsonObject>()) db_.to<JsonObject>();
  JsonObject o = db_[ns].is<JsonObject>() ? db_[ns].as<JsonObject>() : db_[ns].to<JsonObject>();
  std::vector<std::string> parts = split(key, '.');
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    if (!o[parts[i]].is<JsonObject>()) o[parts[i]].to<JsonObject>();
    o = o[parts[i]].as<JsonObject>();
  }
  o[parts.back()] = p["value"];
  db_dirty_ = true;
  db_changed_ = now_;
  r["namespace"] = ns;
  r["key"] = key;
  r["value"] = p["value"];
  return 0;
}

int Service::m_db_delete(const Call&, JsonVariantConst p, JsonDocument& r, std::string& err) {
  std::string ns = str(p["namespace"]), key = db_key(p["key"]);
  if (ns.empty() || key.empty()) return err = "missing namespace or key", kBadRequest;
  JsonObject o = db_[ns].as<JsonObject>();
  std::vector<std::string> parts = split(key, '.');
  for (size_t i = 0; !o.isNull() && i + 1 < parts.size(); i++) o = o[parts[i]].as<JsonObject>();
  if (o.isNull() || o[parts.back()].isNull())
    return err = "Key '" + key + "' in namespace '" + ns + "' not found", kNotFound;
  r["namespace"] = ns;
  r["key"] = key;
  r["value"] = o[parts.back()];
  o.remove(parts.back());
  if (db_[ns].as<JsonObject>().size() == 0) db_.remove(ns);
  db_dirty_ = true;
  db_changed_ = now_;
  return 0;
}

void Service::db_load() {
  db_.to<JsonObject>();
  if (platform_.db_path.empty()) return;
  FILE* f = fopen(platform_.db_path.c_str(), "rb");
  if (!f) return;
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  fclose(f);
  if (deserializeJson(db_, text, DeserializationOption::NestingLimit(32)) || !db_.is<JsonObject>())
    db_.to<JsonObject>();
}

void Service::db_save() {
  db_dirty_ = false;
  if (platform_.db_path.empty()) return;
  std::string text = to_json(db_), tmp = platform_.db_path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fclose(f) == 0 && ok;
  if (!ok) {
    ::remove(tmp.c_str());
    return;
  }
  ::remove(platform_.db_path.c_str());  // FAT's rename does not replace
  ::rename(tmp.c_str(), platform_.db_path.c_str());
}

}  // namespace halyard
