// SPDX-License-Identifier: GPL-3.0-or-later
// A simulated Marlin 2.1 serial port, modelled on Marlin/src/gcode/queue.cpp and friends
// (2.1.2.8), for testing the link layer on the host. It reproduces the behaviour the link
// depends on, not the whole firmware:
//   - line numbers and checksums; an error empties the RX buffer, then "Resend"/"ok"
//   - a repeat of line last_N or last_N-1 is dropped silently
//   - BUFSIZE command slots; "ok" after the command runs; ADVANCED_OK's "ok N P B"
//   - M28 saving mode (numbered lines written with N and checksum stripped) and M29
//   - media printing, M27 progress, M524 abort (clears the queue, injects G28XY)
//   - the emergency parser, busy keepalives, M155/M27 auto-reports, M112 kill
//   - heater targets (M104/M140), and M28 accepting only 8.3 names without LFN_WRITE
//   - M303 PID autotune (the port unread for its duration, then queue.flush_rx()), and a
//     BLTouch-style probe: M851, G30, M420, M211 S, with the probe's true offset modelled
//   - binary transfer (M28 B1): packets, sync, resend, heatshrink via Marlin's own decoder,
//     the file-transfer protocol, and binary mode lasting until a control CLOSE
// Both directions of the wire run at 250000 baud and can corrupt bytes at a set rate.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <random>
#include <string>
#include <vector>

#include "../heatshrink/heatshrink_decoder.cpp"  // Marlin's decoder, one translation unit
#include "../heatshrink/heatshrink_decoder.h"
#include "link.h"

struct FakeMarlin {
  struct Options {
    bool advanced_ok = true;
    bool emergency_parser = true;
    bool autoreport_temp = true;
    bool autoreport_sd = true;
    bool long_filename = true;
    bool m20_timestamp = true;   // M20_TIMESTAMP_SUPPORT: M20 T adds the file's date
    bool keepalive = true;
    bool abort_homes_xy = true;  // EVENT_GCODE_SD_ABORT "G28XY"
    bool binary = false;         // BINARY_FILE_TRANSFER
    bool busy_while_heating = true;  // busy keepalives during M303 (and M109/M190)
    bool pid_bed = true;         // PIDTEMPBED
    bool probe = true;           // a bed probe (Cap:Z_PROBE), homing Z with it
    bool leveling = true;        // M420
    bool lin_advance = true;     // LIN_ADVANCE (M900); M115 has no capability for it
    bool babystep = true;        // BABYSTEPPING (M290, Cap:BABYSTEPPING)
    bool babystep_zprobe = true; // BABYSTEP_ZPROBE_OFFSET: M290 also moves M851 Z, unless P0
    bool limits_report = true;   // M201/M203/M204 without parameters report (Marlin 2.1)
    double max_fr_edit = 0;      // LIMITED_MAX_FR_EDITING holds M203 to this; 0: not built
    bool runout = false;         // FILAMENT_RUNOUT_SENSOR: M119 reports it too
    bool host_actions = true;    // HOST_ACTION_COMMANDS: M25 says //action:pause, M24 resume
    bool eeprom = true;          // EEPROM_SETTINGS (Cap:EEPROM): without it, no M500 or M501
    bool m503 = true;            // M503 reports the settings (off: DISABLE_M503)
    std::string machine = "3D Printer";  // MACHINE_TYPE in M115: CUSTOM_MACHINE_NAME
    // Marlin up to 2.1.2.x: diveToFile() copies each folder name of an M23/M30 path with
    // strncpy and no terminator, so the folder opened is the name plus whatever follows it
    // on the stack, and a file in a folder can't be opened or deleted. bugfix-2.1.x fixes it.
    std::string version = "2.1.2.8";
    bool folder_bug = true;
    size_t max_cmd = 96;         // MAX_CMD_SIZE: the line buffer, and the binary payload limit
    int bufsize = 4;
    size_t rx_size = 1024;
    uint32_t baud = 250000;
  } opt;

  // Marlin 2.1.2.8 as it ships (Configuration.h and Configuration_adv.h untouched), plus what
  // Halyard requires: SD support, on the serial port Halyard is wired to. Stock Marlin
  // auto-reports temperatures, sends busy keepalives, has hotend PID and reports its limits
  // (M201/M203/M204), and has none of the rest: no EEPROM, probe, leveling, babystepping,
  // Linear Advance, bed PID, host actions, long names, binary transfer, SD auto-report,
  // ADVANCED_OK or emergency parser. RX_BUFFER_SIZE is left at the core's default.
  static Options stock() {
    Options o;
    o.advanced_ok = o.emergency_parser = o.autoreport_sd = false;
    o.long_filename = o.m20_timestamp = o.binary = false;
    o.pid_bed = o.probe = o.leveling = o.lin_advance = false;
    o.babystep = o.babystep_zprobe = o.host_actions = o.eeprom = false;
    o.rx_size = 128;
    return o;
  }

  // Fault injection: chance per byte of a flipped bit, per direction.
  double h2p_error = 0, p2h_error = 0;
  std::mt19937 rng{12345};

  // Wire, 25 bytes per ms each way at 250000 baud.
  std::deque<uint8_t> wire_in, wire_out, host_rx;
  uint32_t host_baud = 250000;

  // Printer state.
  std::deque<uint8_t> rx;
  std::string line;
  bool eol = false, esc = false;
  long last_N = 0;
  struct Cmd {
    std::string text;
    bool host;
  };
  std::deque<Cmd> queue;
  bool executing = false;
  uint32_t busy_until = 0, last_busy = 0;
  uint32_t heat_ms = 1000;  // M109/M190's wait
  bool saving = false;
  std::string save_name, save_data;
  // Files the printer wrote itself (M28): with no clock it dates them FAT_DEFAULT_DATE and
  // FAT_DEFAULT_TIME, 2000-01-01 01:00 (sd/SdBaseFile.h).
  std::set<std::string> written;
  std::map<std::string, std::string> files;  // upper-case 8.3 name -> content
  bool sd_printing = false, sd_paused = false, abort_soon = false, sd_eof = false;
  std::string selected;
  size_t sd_pos = 0;
  uint32_t temp_interval = 0, sd_interval = 0, last_temp = 0, last_sd = 0;
  long hotend_target = 0, bed_target = 0;
  int hotend_power = 0, bed_power = 0;  // the heaters' soft PWM, 0 to 127
  bool killed = false;
  // M303 and the probe. Z positions are Marlin's coordinates; the bed's surface at the
  // centre is bed_bump above where a correct offset puts Z0.
  bool autotune = false;
  uint32_t last_tune_report = 0;
  std::vector<std::string> tune_result;
  double probe_z = -0.9, true_probe_z = -1.25, bed_bump = 0.05;
  double home_probe_z = -0.9;  // the offset Z was homed with
  double z_pos = 0;
  bool soft_endstops = true, leveling_on = true;
  double advance_k = 0.22;
  // The test printer's configuration: DEFAULT_MAX_FEEDRATE, _MAX_ACCELERATION, _ACCELERATION.
  double max_feedrate[2] = {500, 500}, max_accel[2] = {3000, 3000};
  double accel = 3000, travel_accel = 3000;
  long feedrate_pct = 100, flow_pct = 100;  // M220, M221 (the printer's Tune menu too)
  bool homed[3] = {false, false, false};  // axis_was_homed(): only G28 or a reset change it
  double babystep_z = 0;                  // what M290 moved, on top of z_pos
  // M119's switches that read TRIGGERED: "x_min", "y_min", "z_min", "z_probe", "filament".
  std::vector<std::string> triggered;
  int settings_saved = 0;
  bool fail_open = false;  // M28 answers "open failed" (a full or write-protected card)
  bool unplugged = false;  // the wire is cut both ways; the printer carries on regardless

  // Binary mode (feature/binary_stream.h).
  bool binary_mode = false, bin_active = false, bin_compress = false;
  bool bin_ioerror = false;       // fault injection: WRITE fails
  enum class Bs { Wait, Header, Data, Footer } bs = Bs::Wait;
  uint8_t bin_sync = 0, bin_hdr[8] = {}, bin_retries = 0;
  size_t bin_got = 0;
  std::string bin_data;
  uint16_t bin_cs = 0, bin_hdr_cs = 0;
  uint8_t bin_foot[2] = {};
  uint32_t bin_last_byte = 0, bin_transfer_at = 0;
  std::string bin_name, bin_file;
  heatshrink_decoder hsd;
  bool mounted = true;  // card.release() after a binary transfer unmounts it until M21
  bool bin_dummy = false;
  bool bin_saving = false;     // OPEN went through openFileWrite(): flag.saving is set too
  bool bin_file_open = false;  // the card's file is open for writing
  int bin_aborts = 0;   // file-transfer ABORTs received; Halyard must never send one
  // card.filename: the file the card last touched. openFileWrite, M23, every M20 entry
  // and the printer's own file menu all overwrite it; transfer_abort() deletes it.
  std::string card_filename;
  int bin_packets = 0, bin_bad = 0;
  std::string ep_line;
  uint32_t now = 0;

  // What happened, for assertions.
  std::vector<std::string> executed;  // commands run as G-code, host and media
  int homes = 0, rx_overflows = 0, errors = 0;

  // ---- wire -------------------------------------------------------------------------------

  uint8_t maybe_corrupt(uint8_t b, double p) {
    if (p > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < p) b ^= 1 << (rng() % 8);
    return b;
  }
  void host_write(const uint8_t* b, size_t n) { wire_in.insert(wire_in.end(), b, b + n); }
  size_t host_read(uint8_t* b, size_t max) {
    size_t n = 0;
    while (n < max && !host_rx.empty()) {
      b[n++] = host_rx.front();
      host_rx.pop_front();
    }
    return n;
  }
  void out(const std::string& s) {
    for (char c : s) wire_out.push_back((uint8_t)c);
    wire_out.push_back('\n');
  }
  void deliver() {
    if (unplugged) {
      wire_in.clear();
      wire_out.clear();
      return;
    }
    bool match = host_baud == opt.baud;
    for (int i = 0; i < 25 && !wire_in.empty(); i++) {
      uint8_t b = wire_in.front();
      wire_in.pop_front();
      if (killed) continue;
      b = match ? maybe_corrupt(b, h2p_error) : (uint8_t)(0x80 | (b * 7));
      if (rx.size() >= opt.rx_size) {
        rx_overflows++;
        continue;
      }
      rx.push_back(b);
      emergency_parser(b);
    }
    for (int i = 0; i < 25 && !wire_out.empty(); i++) {
      uint8_t b = wire_out.front();
      wire_out.pop_front();
      host_rx.push_back(match ? maybe_corrupt(b, p2h_error) : (uint8_t)(0x80 | (b * 7)));
    }
  }

  void emergency_parser(uint8_t c) {
    // Marlin disables it while a file is open for writing (openFileWrite ... closefile).
    if (!opt.emergency_parser || saving || bin_active) return;
    if (c != '\n' && c != '\r') {
      if (ep_line.size() < 100) ep_line += (char)c;
      return;
    }
    const char* p = ep_line.c_str();
    while (*p == ' ') p++;
    if (*p == 'N') {
      p++;
      while (isdigit((unsigned char)*p) || *p == ' ' || *p == '-') p++;
    }
    auto is = [&](const char* m) {
      size_t n = strlen(m);
      return strncmp(p, m, n) == 0 && !isdigit((unsigned char)p[n]);
    };
    if (is("M524") && sd_printing) abort_soon = true;
    if (is("M112")) kill();
    ep_line.clear();
  }

  // ---- queue.cpp ----------------------------------------------------------------------------

  void line_error(const char* msg) {
    errors++;
    out(std::string("Error:") + msg + std::to_string(last_N));
    rx.clear();  // "Clear out the RX buffer"
    out("Resend: " + std::to_string(last_N + 1));
    out("ok");
  }

  static bool is_m29(const std::string& s) {
    size_t p = s.find("M29");
    return p != std::string::npos && !isdigit((unsigned char)(p + 3 < s.size() ? s[p + 3] : 0));
  }

  void get_serial_commands() {
    while ((int)queue.size() < opt.bufsize && !rx.empty()) {
      char c = (char)rx.front();
      rx.pop_front();
      if (c == '\n' || c == '\r') {
        eol = esc = false;
        if (line.empty()) continue;
        std::string cmd = line;
        line.clear();
        size_t s = cmd.find_first_not_of(' ');
        cmd = s == std::string::npos ? "" : cmd.substr(s);
        if (!cmd.empty() && cmd[0] == 'N') {
          bool m110 = cmd.find("M110") != std::string::npos;
          const char* npos = cmd.c_str();
          if (m110) {
            const char* n2 = strchr(cmd.c_str() + 4, 'N');
            if (n2) npos = n2;
          }
          long n = strtol(npos + 1, nullptr, 10);
          if (n != last_N + 1 && !m110) {
            if (n >= last_N - 1 && n <= last_N) continue;  // "two - oops!"
            line_error("Line Number is not Last Line Number+1, Last Line: ");
            return;
          }
          size_t a = cmd.rfind('*');
          if (a == std::string::npos) {
            line_error("No Checksum with line number, Last Line: ");
            return;
          }
          uint8_t cs = 0;
          for (size_t i = 0; i < a; i++) cs ^= (uint8_t)cmd[i];
          if (strtol(cmd.c_str() + a + 1, nullptr, 10) != cs) {
            line_error("checksum mismatch, Last Line: ");
            return;
          }
          last_N = n;
        } else if (saving && !is_m29(cmd)) {
          line_error("No Checksum with line number, Last Line: ");
          return;
        }
        queue.push_back({cmd, true});
      } else {
        if (eol) continue;
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
          continue;
        } else if (c == ';') {
          eol = true;
          continue;
        }
        line += c;
        if (line.size() >= 95) eol = true;
      }
    }
  }

  // Command text without "N<n> " and "*<cs>".
  static std::string bare(const std::string& t) {
    std::string s = t;
    if (!s.empty() && s[0] == 'N') {
      size_t sp = s.find(' ');
      s = sp == std::string::npos ? "" : s.substr(sp + 1);
    }
    size_t a = s.find('*');
    if (a != std::string::npos) s = s.substr(0, a);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  }

  void ok_for(const Cmd& c) {
    if (!c.host) return;
    if (!opt.advanced_ok) return out("ok");
    std::string o = "ok";
    if (!c.text.empty() && c.text[0] == 'N') o += " " + c.text.substr(0, c.text.find(' '));
    o += " P15 B" + std::to_string(opt.bufsize - (int)queue.size());
    out(o);
  }

  // ---- cardreader and gcode handlers ------------------------------------------------------

  static std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
  }
  std::string arg(const std::string& b) const {
    size_t sp = b.find(' ');
    return sp == std::string::npos ? "" : b.substr(sp + 1);
  }
  bool param(const std::string& b, char k, long& v) const {
    for (size_t i = 1; i < b.size(); i++)
      if (b[i] == k && b[i - 1] == ' ') {
        v = strtol(b.c_str() + i + 1, nullptr, 10);
        return true;
      }
    return false;
  }
  bool fparam(const std::string& b, char k, double& v) const {
    for (size_t i = 1; i < b.size(); i++)
      if (b[i] == k && b[i - 1] == ' ') {
        v = strtod(b.c_str() + i + 1, nullptr);
        return true;
      }
    return false;
  }
  static std::string fixed(double v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
  }
  // Where the nozzle is above the bed's surface at the centre, in mm.
  double nozzle_height() const { return z_pos + home_probe_z - true_probe_z - bed_bump; }
  static bool is_83(const std::string& n) {
    size_t dot = n.find('.');
    std::string base = n.substr(0, dot), ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    return !base.empty() && base.size() <= 8 && ext.size() <= 3 && ext.find('.') == std::string::npos;
  }
  std::string temps() const {
    return " T:24.00 /" + std::to_string(hotend_target) + ".00 B:23.00 /" +
           std::to_string(bed_target) + ".00 @:" + std::to_string(hotend_power) +
           " B@:" + std::to_string(bed_power);
  }
  void report_sd() {
    if (sd_printing)
      out("SD printing byte " + std::to_string(sd_pos) + "/" +
          std::to_string(files[selected].size()));
    else
      out("Not SD printing");
  }
  void abort_print() {  // MarlinCore abortSDPrinting()
    sd_printing = sd_paused = abort_soon = false;
    queue.clear();
    executing = false;
    busy_until = 0;
    if (opt.abort_homes_xy) queue.push_back({"G28XY", false});
  }

  // Returns false if the handler printed its own ok (M105) or must not print one.
  bool execute(const Cmd& c) {
    std::string b = bare(c.text);
    executed.push_back(b);
    std::string code = b.substr(0, b.find(' '));
    long v = 0;
    if (code == "M110") {
      if (param(b, 'N', v)) last_N = v;
    } else if (code == "M115") {
      out("FIRMWARE_NAME:Marlin " + opt.version + " (fake) SOURCE_CODE_URL:github.com/MarlinFirmware/Marlin "
          "PROTOCOL_VERSION:1.0 MACHINE_TYPE:" + opt.machine + " EXTRUDER_COUNT:1 UUID:00000000-0000-4000-8000-000000000000");
      auto cap = [&](const char* n, bool on) { out(std::string("Cap:") + n + ":" + (on ? "1" : "0")); };
      cap("SERIAL_XON_XOFF", false);
      cap("BINARY_FILE_TRANSFER", opt.binary);
      cap("AUTOREPORT_TEMP", opt.autoreport_temp);
      cap("AUTOREPORT_SD_STATUS", opt.autoreport_sd);
      cap("EMERGENCY_PARSER", opt.emergency_parser);
      cap("LONG_FILENAME", opt.long_filename);
      cap("EXTENDED_M20", opt.long_filename);
      cap("LFN_WRITE", opt.long_filename);
      cap("SDCARD", true);
      cap("Z_PROBE", opt.probe);
      cap("LEVELING_DATA", opt.leveling);
      cap("BABYSTEPPING", opt.babystep);
      cap("SD_WRITE", true);
      cap("EEPROM", opt.eeprom);
    } else if (code == "M105") {
      if (c.host) out("ok" + temps());
      return false;
    } else if (code == "M104") {
      if (param(b, 'S', v)) hotend_target = v;
    } else if (code == "M140") {
      if (param(b, 'S', v)) bed_target = v;
    } else if (code == "M155") {
      if (opt.autoreport_temp && param(b, 'S', v)) temp_interval = v * 1000;
    } else if (code == "M27") {
      if (b.find(" C") != std::string::npos)
        out("Current file: " + (selected.empty() ? std::string("(no file)") : selected));
      else if (opt.autoreport_sd && param(b, 'S', v))
        sd_interval = v * 1000;
      else
        report_sd();
    } else if (code == "M21") {
      mounted = true;
      out("echo:SD card ok");
    } else if ((code == "M20" || code == "M23" || code == "M28" || code == "M30") && !mounted &&
               !(code == "M28" && arg(b).size() >= 2 && arg(b)[0] == 'B')) {
      if (code == "M20" || code == "M23") out("echo:No media");  // M28, M30: nothing at all
    } else if (code == "M20") {
      out("Begin file list");
      for (auto& f : files) {
        card_filename = f.first;  // printListing() -> createFilename(filename, ...)
        std::string l = f.first + " " + std::to_string(f.second.size());
        if (opt.m20_timestamp && b.find(" T") != std::string::npos)
          l += written.count(f.first) ? " 0x28210800" : " 0x5B2A6C00";
        if (opt.long_filename && b.find(" L") != std::string::npos) l += " " + f.first;
        out(l);
      }
      out("End file list");
    } else if ((code == "M23" || code == "M30") && opt.folder_bug && arg(b).find('/') != std::string::npos) {
      std::string n = arg(b);
      out("echo:open failed, File: " + n.substr(0, n.find('/')) + "WWW.");  // and stack garbage
    } else if (code == "M23") {
      std::string n = upper(arg(b));
      if (files.count(n)) {
        selected = n;
        card_filename = n;
        sd_pos = 0;
        sd_eof = false;
        out("File opened: " + n + " Size: " + std::to_string(files[n].size()));
        out("File selected");
      } else {
        out("open failed, File: " + arg(b) + ".");
      }
    } else if (code == "M24") {  // also from the printer's own menu (MarlinUI::resume_print)
      if (!selected.empty()) sd_printing = true, sd_paused = false;
      if (opt.host_actions) out("//action:resume");
    } else if (code == "M25") {  // also from the printer's own menu (MarlinUI::pause_print)
      sd_paused = true;
      if (opt.host_actions) out("//action:pause");
    } else if (code == "M28" && opt.binary && arg(b).size() >= 2 && arg(b)[0] == 'B' &&
               isdigit((unsigned char)arg(b)[1])) {
      binary_mode = arg(b)[1] > '0';
      if (binary_mode) {
        out("echo:Switching to Binary Protocol");
        bs = Bs::Wait;
      }
    } else if (code == "M28") {
      std::string n = upper(arg(b));
      if (sd_printing) sd_printing = sd_paused = false;  // openFileWrite() -> abortFilePrintNow()
      if (n.empty() || (!opt.long_filename && !is_83(n)) || fail_open) {
        out("open failed, File: " + n + ".");
      } else {
        saving = true;
        card_filename = n;
        save_name = n;
        written.insert(n);
        save_data.clear();
        out("echo:Now fresh file: " + n);
        out("Writing to file: " + n);
      }
    } else if (code == "M29") {
      saving = false;
    } else if (code == "M30") {
      // gcode/sd/M30.cpp: card.closefile() first, so a running print loses its file.
      if (sd_printing) sd_printing = sd_paused = false;
      std::string n = upper(arg(b));
      if (files.erase(n)) out("File deleted:" + n);
      else out("Deletion failed, File: " + n + ".");
    } else if (code == "M524") {
      if (sd_printing) abort_soon = true;
    } else if (code == "M112") {
      kill();
      return false;
    } else if (code == "M109" || code == "M190") {  // waits; heat_ms is how long it takes
      if (param(b, 'S', v)) (code == "M109" ? hotend_target : bed_target) = v;
      busy_until = now + heat_ms;
    } else if (code == "G4") {
      if (param(b, 'S', v)) busy_until = now + v * 1000;
      else if (param(b, 'P', v)) busy_until = now + v;
    } else if (code == "G28" || code == "G28XY") {
      homes++;
      std::string axes = code == "G28XY" ? "XY" : b.substr(3);
      bool any = axes.find_first_of("XYZ") != std::string::npos;
      for (int i = 0; i < 3; i++)
        if (!any || axes.find("XYZ"[i]) != std::string::npos) homed[i] = true;
      if (code == "G28" && opt.probe) {
        home_probe_z = probe_z;
        z_pos = 10;
      }
    } else if (code == "M211") {  // M211_report(): no heading, no "echo:" (control/M211.cpp)
      if (param(b, 'S', v)) {
        soft_endstops = v != 0;
      } else {
        out(soft_endstops ? "  M211 S1 ; ON" : "  M211 S0 ; OFF");
        out("  Min:  X0.00 Y0.00 Z0.00   Max:  X235.00 Y235.00 Z250.00");
      }
    } else if (code == "M303") {  // gcode/temp/M303.cpp, Temperature::PID_autotune()
      long e = 0, t = 0;
      param(b, 'E', e);
      if (e == -1 && !opt.pid_bed) {
        out("PID Autotune failed! Bad heater id");
      } else {
        param(b, 'S', t);
        out("PID Autotune start");
        autotune = true;
        last_tune_report = now;
        busy_until = now + 20000;
        std::string pre = e == -1 ? "#define DEFAULT_BED_" : "#define DEFAULT_";
        tune_result = {"PID Autotune finished! Put the last Kp, Ki and Kd constants from below "
                       "into Configuration.h",
                       pre + "KP 97.10", pre + "KI 1.41", pre + "KD 1663.66"};
      }
    } else if (code == "M851" && opt.probe) {  // M851_report(), for replay: no "echo:"
      double z;
      if (fparam(b, 'Z', z)) probe_z = z;
      else out("  M851 X-33.00 Y-15.00 Z" + fixed(probe_z) + " ; (mm)");
    } else if (code == "G30" && opt.probe) {
      leveling_on = false;
      // Measured: the surface in coordinates made with the offset Z was homed with.
      out("Bed X: 117.50 Y: 117.50 Z: " + fixed(bed_bump + probe_z - home_probe_z));
      z_pos = 10;
    } else if (code == "M420" && opt.leveling) {
      if (param(b, 'S', v)) leveling_on = v != 0;
      out(std::string("echo:Bed Leveling ") + (leveling_on ? "ON" : "OFF"));
    } else if (code == "M900" && opt.lin_advance) {  // feature/advance/M900.cpp
      double k;
      if (fparam(b, 'K', k)) {
        if (k >= 0 && k <= 10) advance_k = k;
        else out("?K value out of range (0-10).");
      } else if (b == "M900") {
        out("echo:Advance K=" + fixed(advance_k));
      }
    } else if (code == "M201" || code == "M203") {  // gcode/config/M200-M205.cpp
      double* v = code == "M201" ? max_accel : max_feedrate;
      double x;
      bool any = false;
      for (int i = 0; i < 2; i++) {
        if (!fparam(b, "XY"[i], x)) continue;
        any = true;
        if (code == "M203" && opt.max_fr_edit > 0) x = std::min(x, opt.max_fr_edit);
        v[i] = x;
      }
      // M*_report() for replay: no heading, no "echo:"
      if (!any && opt.limits_report)
        out("  " + code + " X" + fixed(v[0]) + " Y" + fixed(v[1]) + (code == "M201" ? " Z100.00 E10000.00" : " Z5.00 E25.00"));
    } else if (code == "M204") {
      double x;
      bool any = false;
      if (fparam(b, 'S', x)) accel = travel_accel = x, any = true;
      if (fparam(b, 'P', x)) accel = x, any = true;
      if (fparam(b, 'T', x)) travel_accel = x, any = true;
      if (fparam(b, 'R', x)) any = true;
      if (!any && opt.limits_report)
        out("  M204 P" + fixed(accel) + " R" + fixed(accel) + " T" + fixed(travel_accel));
    } else if (code == "M220") {  // gcode/config/M220.cpp
      if (param(b, 'S', v)) feedrate_pct = v;
      else if (b == "M220") out("FR:" + std::to_string(feedrate_pct) + "%");
    } else if (code == "M221") {  // gcode/config/M221.cpp
      if (param(b, 'S', v)) flow_pct = v;
      else if (b == "M221") out("echo:E0 Flow: " + std::to_string(flow_pct) + "%");
    } else if (code == "M119") {  // Endstops::report_states(): no "echo:"
      out("Reporting endstop status");
      std::vector<std::string> names = {"x_min", "y_min", "z_min"};
      if (opt.probe) names.push_back("z_probe");
      if (opt.runout) names.push_back("filament");
      for (auto& n : names) {
        bool hit = false;
        for (auto& t : triggered) hit |= t == n;
        out(n + ": " + (hit ? "TRIGGERED" : "open"));
      }
    } else if (code == "M290" && opt.babystep) {  // gcode/motion/M290.cpp; never sent bare
      double z;
      long p = 1;
      param(b, 'P', p);
      if (fparam(b, 'Z', z) && homed[2]) {  // Babystep::add_steps: not on an unhomed axis
        z = std::max(-2.0, std::min(2.0, z));
        babystep_z += z;
        if (opt.babystep_zprobe && p) probe_z += z;
      }
    } else if (code == "M114") {  // report_current_position_projected()
      out("X:0.00 Y:0.00 Z:" + fixed(z_pos) + " E:0.00 Count X:0 Y:0 Z:" +
          std::to_string((long)(z_pos * 400)));
    } else if (code == "M503" && opt.m503) {  // MarlinSettings::report(), with headings
      auto head = [&](const std::string& h) { out("echo:; " + h + ":"); };
      auto line = [&](const std::string& l) { out("echo:  " + l); };
      head("Linear Units");
      line("G21 ; (mm)");
      head("Temperature Units");
      line("M149 C ; Units in Celsius");
      head("Steps per unit");
      line("M92 X80.00 Y80.00 Z400.00 E93.00");
      head("Max feedrates (units/s)");
      line("M203 X" + fixed(max_feedrate[0]) + " Y" + fixed(max_feedrate[1]) + " Z5.00 E25.00");
      head("Max Acceleration (units/s2)");
      line("M201 X" + fixed(max_accel[0]) + " Y" + fixed(max_accel[1]) + " Z100.00 E10000.00");
      head("Acceleration (units/s2) (P<print-accel> R<retract-accel> T<travel-accel>)");
      line("M204 P" + fixed(accel) + " R" + fixed(accel) + " T" + fixed(travel_accel));
      head("Advanced (B<min_segment_time_us> S<min_feedrate> T<min_travel_feedrate> J<junc_dev>)");
      line("M205 B20000.00 S0.00 T0.00 J0.08");
      head("Home offset");
      line("M206 X0.00 Y0.00 Z0.00");
      if (opt.leveling) {
        head("Auto Bed Leveling");
        line(std::string("M420 S") + (leveling_on ? "1" : "0") + " Z10.00 ; Leveling " + (leveling_on ? "ON" : "OFF"));
        line("G29 W I0 J0 Z0.12000");
        line("G29 W I1 J0 Z-0.04000");
      }
      head("Hotend PID");
      line("M301 P21.73 I1.54 D76.55");
      if (opt.pid_bed) {
        head("Bed PID");
        line("M304 P97.10 I1.41 D1663.66");
      }
      if (opt.probe) {
        head("Z-Probe Offset");
        line("M851 X-33.00 Y-15.00 Z" + fixed(probe_z) + " ; (mm)");
      }
      if (opt.lin_advance) {
        head("Linear Advance");
        line("M900 K" + fixed(advance_k));
      }
    } else if (code == "M500" && opt.eeprom) {
      settings_saved++;
      out("echo:Settings Stored (628 bytes; crc 12345)");
    } else if (code == "G0" || code == "G1") {
      double z;
      if (fparam(b, 'Z', z)) z_pos = soft_endstops && z < 0 ? 0 : z;
    } else if (code == "G90" || code == "G91" || code == "M82" || code == "M83") {
    } else if (code == "M117" || code == "M84" || code == "M400") {
    } else if (c.host) {  // media lines are real G-code (G1, M106...); accept them quietly
      out("echo:Unknown command: \"" + b + "\"");
    }
    return true;
  }

  void kill() {
    out("Error:Printer halted. kill() called!");
    killed = true;
  }

  // Power cycle or reset button.
  void reboot() {
    *this = FakeMarlin(opt, files, h2p_error, p2h_error, now);
    out("start");
    out("echo:Marlin 2.1.2.8");
  }
  FakeMarlin() = default;
  FakeMarlin(Options o, std::map<std::string, std::string> f, double e1, double e2, uint32_t t)
      : opt(o), h2p_error(e1), p2h_error(e2), files(std::move(f)), now(t) {}

  // ---- MarlinCore loop ----------------------------------------------------------------------

  // ---- binary_stream.h ----------------------------------------------------------------------

  static uint16_t fl16(uint16_t cs, uint8_t v) {
    uint16_t lo = (uint16_t)(((cs & 0xFF) + v) % 255);
    return (uint16_t)(((((cs >> 8) + lo) % 255) << 8) | lo);
  }
  void bin_resend() {
    bin_retries++;  // MAX_RETRIES 0: unlimited
    out("echo:Resend request " + std::to_string(bin_retries));
    out("rs" + std::to_string(bin_sync));
    bs = Bs::Wait;
  }
  void release() {  // CardReader::release()
    mounted = false;
    out("echo:SD card released");
  }
  void bin_abort_file() {  // SDFileTransferProtocol::transfer_abort()
    bin_active = false;
    bin_file.clear();
    if (bin_dummy) return;
    saving = bin_saving = bin_file_open = false;
    files.erase(card_filename);  // removeFile(card.filename): whatever that is by now
    release();
  }
  void bin_dispatch(uint8_t meta) {
    uint8_t protocol = meta >> 4, type = meta & 0xF;
    if (protocol == 0) {
      if (type == 2) binary_mode = false;  // CLOSE: back to ASCII
      else out("echo:Unknown BinaryProtocolControl Packet");
      return;
    }
    if (protocol != 1) return out("echo:Unsupported Binary Protocol");
    bin_transfer_at = now;
    switch (type) {
      case 0:
        out("PFT:version:0.1.0:compression:heatshrink,8,4");
        break;
      case 1: {  // OPEN
        if (bin_active) { out("PFT:busy"); break; }
        if (bin_data.size() <= 2 || bin_data.back() != '\0') { out("PFT:fail"); break; }
        std::string n = upper(bin_data.substr(2, bin_data.size() - 3));
        bin_dummy = bin_data[0] & 1;
        if (bin_dummy) {  // file_open() with dummy_transfer: no card at all
          bin_active = true;
          bin_compress = bin_data[1] & 1;
          heatshrink_decoder_reset(&hsd);
          bin_file.clear();
          out("PFT:success");
          break;
        }
        if (n.empty() || (!opt.long_filename && !is_83(n)) || fail_open) {
          out("open failed, File: " + n + ".");
          out("PFT:fail");
          break;
        }
        if (!mounted) {  // file_open() mounts first
          mounted = true;
          out("echo:SD card ok");
        }
        if (sd_printing) sd_printing = sd_paused = false;  // openFileWrite() aborts a print
        out("echo:Now fresh file: " + n);
        out("Writing to file: " + n);
        bin_name = n;
        card_filename = n;
        saving = bin_saving = bin_file_open = true;  // openFileWrite() sets flag.saving
        save_name = n;
        written.insert(n);
        bin_file.clear();
        files[n].clear();  // O_CREAT | O_TRUNC: the file exists from here on
        bin_compress = bin_data[1] & 1;
        heatshrink_decoder_reset(&hsd);
        bin_active = true;
        out("PFT:success");
        break;
      }
      case 2:  // CLOSE
        if (!bin_active) { out("PFT:invalid"); break; }
        if (!bin_dummy && bin_compress && !bin_file_open) {
          // file_close() flushes the decoder's pending bytes with card.write(), which fails
          // on a closed file: ioerror, and transfer_active stays set for good.
          out("PFT:ioerror");
          break;
        }
        if (bin_compress) bin_drain(true);
        bin_active = false;
        if (!bin_dummy) {  // file_close(): closefile(), release()
          files[bin_name] = bin_file;
          saving = bin_saving = bin_file_open = false;
          release();
        }
        out("PFT:success");
        break;
      case 3:  // WRITE
        if (!bin_active) { out("PFT:invalid"); break; }
        if (bin_ioerror) { out("PFT:ioerror"); break; }
        if (!bin_compress) {
          bin_file += bin_data;
          if (!bin_dummy) files[bin_name] = bin_file;  // written as it arrives
          break;
        }
        {
          size_t done = 0, n = 0;
          while (done < bin_data.size()) {
            heatshrink_decoder_sink(&hsd, (uint8_t*)&bin_data[done], bin_data.size() - done, &n);
            done += n;
            bin_drain(false);
          }
          if (!bin_dummy) files[bin_name] = bin_file;
        }
        break;
      case 4:  // ABORT
        bin_aborts++;
        bin_abort_file();
        out("PFT:success");
        break;
      default:
        out("PTF:invalid");
    }
  }
  void bin_drain(bool finish) {
    uint8_t buf[512];
    size_t n = 0;
    if (finish) heatshrink_decoder_finish(&hsd);
    HSD_poll_res r;
    do {
      r = heatshrink_decoder_poll(&hsd, buf, sizeof buf, &n);
      bin_file.append((char*)buf, n);
    } while (r == HSDR_POLL_MORE);
  }
  void bin_byte(uint8_t d) {
    bin_last_byte = now;
    switch (bs) {
      case Bs::Wait:
        bin_hdr[0] = bin_hdr[1];
        bin_hdr[1] = d;
        if (bin_hdr[0] == 0xAD && bin_hdr[1] == 0xB5) {
          bs = Bs::Header;
          bin_got = 2;
          bin_cs = 0;
        }
        return;
      case Bs::Header: {
        bin_hdr[bin_got++] = d;
        bin_cs = fl16(bin_cs, d);
        if (bin_got == 6) bin_hdr_cs = bin_cs;
        if (bin_got < 8) return;
        uint8_t sync = bin_hdr[2], meta = bin_hdr[3];
        uint16_t size = bin_hdr[4] | (bin_hdr[5] << 8);
        uint16_t cs = bin_hdr[6] | (bin_hdr[7] << 8);
        bin_hdr[0] = bin_hdr[1] = 0;
        if (cs != bin_hdr_cs) {
          bin_bad++;
          out("echo:Packet header(" + std::to_string(sync) + "?) corrupt");
          return bin_resend();
        }
        if (meta == 0x01) {  // SYNC, whatever the sequence
          out("ss" + std::to_string(bin_sync) + "," + std::to_string(opt.max_cmd) + ",0.1.0");
          bs = Bs::Wait;
          return;
        }
        if (sync == bin_sync) {
          bin_data.clear();
          bin_got = 0;
          bin_hdr[3] = meta;
          bin_hdr[4] = (uint8_t)size;
          bin_hdr[5] = (uint8_t)(size >> 8);
          bs = size ? Bs::Data : Bs::Wait;
          if (!size) bin_process();
        } else if (sync == (uint8_t)(bin_sync - 1)) {
          out("ok" + std::to_string(sync));  // its ok was lost; drop the payload
          bs = Bs::Wait;
        } else if (bin_retries) {
          bs = Bs::Wait;  // stale packets after a resend request: dropped without a word
        } else {
          out("echo:Datastream packet out of order");
          bin_resend();
        }
        return;
      }
      case Bs::Data: {
        uint16_t size = bin_hdr[4] | (bin_hdr[5] << 8);
        if (bin_data.size() >= opt.max_cmd) {
          out("echo:Datastream packet data buffer overrun");
          out("fe" + std::to_string(bin_hdr[2]));
          bin_sync = 0;
          bin_retries = 0;
          bs = Bs::Wait;
          return;
        }
        bin_data += (char)d;
        bin_cs = fl16(bin_cs, d);
        if (bin_data.size() == size) {
          bs = Bs::Footer;
          bin_got = 0;
        }
        return;
      }
      case Bs::Footer:
        bin_foot[bin_got++] = d;
        if (bin_got < 2) return;
        if ((uint16_t)(bin_foot[0] | (bin_foot[1] << 8)) != bin_cs) {
          bin_bad++;
          out("echo:Packet(" + std::to_string(bin_hdr[2]) + ") payload corrupt");
          return bin_resend();
        }
        bin_process();
        return;
    }
  }
  void bin_process() {
    bin_sync++;
    bin_retries = 0;
    bin_packets++;
    out("ok" + std::to_string((uint8_t)(bin_sync - 1)));
    bs = Bs::Wait;
    bin_dispatch(bin_hdr[3]);
  }
  void bin_receive() {
    // Mid-packet starvation (PACKET_MAX_WAIT 500 ms) means a lost byte: ask again.
    if (bs != Bs::Wait && now - bin_last_byte >= 500) {
      out("echo:Datastream timeout");
      bin_resend();
    }
    while (binary_mode && !rx.empty()) {
      uint8_t d = rx.front();
      rx.pop_front();
      bin_byte(d);
    }
    // An interrupted transfer is dropped after 10 s (TIMEOUT); binary mode stays on.
    if (bin_active && now - bin_transfer_at >= 10000) bin_abort_file();
  }

  void idle() {
    if (autotune) return;  // PID_autotune() loops on its own: no serial, no auto-reports
    if (temp_interval && now - last_temp >= temp_interval) {
      last_temp = now;
      out(temps());
    }
    if (sd_interval && now - last_sd >= sd_interval) {
      last_sd = now;
      report_sd();
    }
    if (binary_mode) bin_receive();
    else get_serial_commands();
    if (sd_printing && !sd_paused && !sd_eof) {  // get_sdcard_commands()
      const std::string& f = files[selected];
      while ((int)queue.size() < opt.bufsize && sd_pos < f.size()) {
        size_t e = f.find('\n', sd_pos);
        if (e == std::string::npos) e = f.size();
        std::string l = f.substr(sd_pos, e - sd_pos);
        sd_pos = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) queue.push_back({l, false});
      }
      if (sd_pos >= f.size()) sd_eof = true;
    }
    if (sd_printing && sd_eof && queue.empty() && !executing) {
      sd_printing = false;
      out("Done printing file");
      queue.push_back({"M84", false});  // SD_FINISHED_RELEASECOMMAND
    }
  }

  // One millisecond.
  void tick(uint32_t t) {
    now = t;
    deliver();
    if (killed) return;
    for (int iter = 0; iter < 20; iter++) {
      idle();
      if (executing) {
        if (now < busy_until) {
          if (autotune && now - last_tune_report >= 2000) {
            last_tune_report = now;
            out(temps());
          }
          if (opt.keepalive && (!autotune || opt.busy_while_heating) && now - last_busy >= 2000) {
            last_busy = now;
            out("echo:busy: processing");
          }
          return;
        }
        executing = false;
        if (autotune) {
          autotune = false;
          for (auto& l : tune_result) out(l);
          rx.clear();  // queue.flush_rx()
          line.clear();
          eol = esc = false;
        }
        if (!queue.empty()) {
          ok_for(queue.front());
          queue.pop_front();
        }
        continue;
      }
      if (abort_soon) abort_print();  // loop(), between commands, not inside idle()
      if (queue.empty()) return;
      Cmd& c = queue.front();
      if (saving && c.host) {  // GCodeQueue::advance()
        if (is_m29(c.text) && bin_saving) {  // closefile() under a binary transfer
          saving = bin_saving = bin_file_open = false;
          out("Done saving file.");
        } else if (is_m29(c.text)) {
          saving = false;
          files[save_name] = save_data;
          out("Done saving file.");
        } else {
          std::string w = c.text;
          if (!w.empty() && w[0] == 'N') {  // CardReader::write_command()
            size_t sp = w.find(' '), star = w.find('*');
            w = w.substr(sp + 1, star - sp - 1);
          }
          save_data += w + "\r\n";
        }
        ok_for(c);
        queue.pop_front();
        continue;
      }
      if (!execute(c)) {  // own ok, or killed
        if (!queue.empty()) queue.pop_front();
        continue;
      }
      if (busy_until > now) {
        executing = true;
        last_busy = now;
        return;
      }
      ok_for(queue.front());
      queue.pop_front();
    }
  }
};

// The link's view of the wire.
struct FakeTransport : halyard::Transport {
  FakeMarlin& m;
  explicit FakeTransport(FakeMarlin& fm) : m(fm) {}
  size_t read(uint8_t* b, size_t max) override { return m.host_read(b, max); }
  size_t write(const uint8_t* b, size_t n) override {
    m.host_write(b, n);
    return n;
  }
  void set_baud(uint32_t baud) override { m.host_baud = baud; }
};
