// SPDX-License-Identifier: GPL-3.0-or-later
// The service layer: the Moonraker subset Mainsail uses (JSON-RPC over a websocket, plus the
// HTTP routes it calls) and OctoPrint's upload endpoints for slicers, on top of the delivery
// layer. It knows nothing about sockets, WiFi or boards: the platform glue hands messages and
// request bodies in, and takes replies and notifications out. Portable C++, no Arduino.
//
// The contract is measured, not guessed: docs/research/mainsail-api-log.md. Mainsail sees a
// Klipper printer, faked from what Marlin reports:
//   - the printer objects Mainsail subscribes to, pushed as notify_status_update deltas;
//   - print start, pause, resume and cancel; a start is a hand-off (D9): preheat, copy to
//     the printer's card if needed, M23/M24;
//   - a console, which translates the Klipper commands Mainsail sends (for example
//     SET_HEATER_TEMPERATURE) and forwards what the printer says;
//   - the file list, metadata and thumbnails from Halyard's library;
//   - a database for Mainsail's settings, saved to a file when a path is given;
//   - halyard.cfg (settings.h) in the config root, for Mainsail's editor, and marlin.cfg
//     beside it: Marlin's settings as M503 reports them, read-only;
//   - Halyard's own: the printer's card (printer.card.*) and the hand-off's progress (the
//     halyard object), which Halyard's Mainsail shows.
#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "delivery.h"
#include "library.h"
#include "link.h"
#include "multipart.h"
#include "printer.h"
#include "zip_stream.h"

namespace halyard {

// What the service needs from the board.
struct Platform {
  std::function<double()> wall_time;                          // Unix seconds, 0 if unknown
  std::function<bool(uint64_t& total, uint64_t& used)> disk;  // Halyard's card
  std::function<bool()> card;  // Halyard's card is mounted; unset means it is
  std::function<uint32_t()> free_memory;                      // bytes
  std::function<float()> cpu_temp;  // the chip's own sensor, °C; unset: none
  std::function<void()> reboot;                               // restart Halyard itself
  std::string hostname = "halyard";
  std::string hardware = "ESP32-S3";
  std::string version = "0.5.0";  // the glue adds the build: "0.5.0-g1a2b3c4"
  std::string os;  // what Halyard runs on: "ESP-IDF v4.4.7, Arduino-ESP32 2.0.17"; "" unknown
  std::string db_path;  // Mainsail's settings are saved here; empty keeps them in memory
  std::string config_path;  // halyard.cfg, the config root's one file; empty: no config root
  std::function<std::string()> ui_version;  // Mainsail's, "v2.19.0"; unset: no update manager
  // The Mainsail this firmware ships with (D13: Halyard's build, "v2.19.0-halyard.1"). The
  // update manager offers it; "" offers nothing.
  std::string ui_release;
  // Halyard's own version for its update manager row, "0.5.0" (no build); "" leaves it out.
  std::string firmware;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

class Service {
 public:
  Service(Link& link, Printer& printer, Delivery& delivery, Library& library, Platform platform);
  ~Service();
  void begin();  // loads the saved database; after the card is mounted

  // Every line shown in Mainsail's console, for the USB console too.
  std::function<void(const std::string&)> on_console;
  // FIRMWARE_RESTART or RESTART (Mainsail's SAVE & RESTART on halyard.cfg): the glue reads
  // halyard.cfg again. The link reconnects as well.
  std::function<void()> on_restart;

  // Problems found in halyard.cfg: shown in Mainsail's console now, and as Moonraker
  // warnings (the notification bell) from then on.
  void set_config_warnings(const std::vector<std::string>& w);
  // A line for the printer's screen (M117): Halyard's address, or what is wrong. Sent when
  // the printer is connected and not printing, and again after it reconnects; "" stops it.
  void show_on_printer(const std::string& text);
  void set_hostname(const std::string& h) { platform_.hostname = h; }  // halyard.cfg's

  // ---- Mainsail's update manager (machine.update.*): "mainsail" and "halyard" entries -------
  // "mainsail" offers Platform::ui_release, the Mainsail Halyard was tested with, not
  // upstream's latest (D13). "halyard" offers the newest release of the firmware, which the
  // check finds. The glue checks (on refresh) and runs updates ("mainsail" or "halyard"),
  // both in the background.
  std::function<void()> on_update_check;
  std::function<bool(const std::string& name, std::string& err)> on_update;
  // After a check: why Platform::ui_release cannot be fetched, or "", and the newest
  // firmware release ("0.6.0"), or "" with why not. Sends notify_update_refreshed.
  void update_checked(const std::string& ui_error, const std::string& release,
                      const std::string& release_error);
  // A line for Mainsail's update dialog (notify_update_response); complete ends the update.
  void update_progress(const std::string& name, const std::string& message, bool complete);

  // ---- websocket: connections are named by an id the glue picks (the socket number) ----
  void ws_open(int conn);
  void ws_close(int conn);
  // A text message: one JSON-RPC request or a batch. Returns what to send back now (may be
  // empty). Replies that wait for the printer (printer.gcode.script) come through take().
  std::string ws_message(int conn, const std::string& text);

  // Notifications and late replies waiting to be sent.
  bool pending() const { return !outbox_.empty(); }
  bool take(int& conn, std::string& text);

  // ---- HTTP -------------------------------------------------------------------------------
  // Every route except uploads and file downloads. `query` is the raw query string. False if
  // nothing matches (a 404).
  bool http(const std::string& method, const std::string& path, const std::string& query,
            const std::string& body, HttpResponse& out);

  // POST /server/files/upload (Moonraker) or /api/files/local (OctoPrint), multipart, fed as
  // it arrives. Returns nullptr with `refused` filled if it cannot start.
  class Upload {
   public:
    ~Upload();
    bool feed(const uint8_t* data, size_t len);  // false: stop reading, finish() says why
    void finish(HttpResponse& out);              // once, after the body (or a failed feed)

   private:
    friend class Service;
    Upload(Service& s, bool octoprint, const std::string& boundary);
    bool part(const std::string& name, const std::string& filename);
    bool data(const uint8_t* d, size_t n);
    bool part_end();
    bool fail(int status, const std::string& msg);

    Service& s_;
    bool octoprint_;
    MultipartParser parser_;
    std::unique_ptr<Library::Writer> writer_;
    bool in_file_ = false, stored_ = false;
    std::string field_, value_, name_;
    bool print_ = false;
    bool cfg_ = false;  // the file is halyard.cfg, kept in cfg_text_ until the root is known
    std::string cfg_text_;
    std::string root_ = "gcodes", path_;
    int status_ = 0;
    std::string error_;
  };
  std::unique_ptr<Upload> upload(bool octoprint, const std::string& content_type,
                                 HttpResponse& refused);

  // The file on Halyard's card for a download URL path (after /server/files/), or "" if
  // there is none: G-code files and their thumbnails.
  std::string download_path(const std::string& path) const;
  // A download made here rather than read from a file: config/halyard.cfg, with the WiFi
  // password masked (settings.h), and config/marlin.cfg. False for every other path.
  bool download_text(const std::string& path, std::string& body) const;
  // The Content-Disposition header for a download (as Moonraker sends it), so a browser
  // saves the file instead of showing it: attachment, named after the path's last part.
  static std::string content_disposition(const std::string& path);
  // The archive server.files.zip promised, when `path` (after /server/files/) is its
  // destination; null for every other path. Made as it downloads, from the files named then.
  std::unique_ptr<ZipStream> zip_download(const std::string& path) const;

  // ---- driven by the glue -----------------------------------------------------------------
  void poll(uint32_t now_ms);                   // after Printer::poll and Delivery::poll
  void printer_line(const std::string& line);  // every line from the printer (Printer::on_line)
  void delivery_event(const std::string& e);   // Delivery::on_event

  // A hand-off, for Mainsail, an upload with print=true, or the USB console. With `print`
  // Mainsail shows it as a print from the start: the copy, then the print.
  bool hand_off(const std::string& name, bool print, std::string& err);
  // A line for Mainsail's console (notify_gcode_response).
  void respond(const std::string& text);  // made ASCII-safe
  const char* klippy_state() const;
  // Without its card Halyard has no library: no uploads, nothing to hand off. The text is a
  // Moonraker warning in server.info, which Mainsail shows under its notification bell.
  bool card_missing() const { return platform_.card && !platform_.card(); }
  static const char* const kNoCard;
  // Firmware updates: whether Halyard may restart now, and restarting after the reply to the
  // current request has gone out (machine.reboot does the same).
  bool can_restart(std::string& why) const;
  void reboot_soon();

 private:
  enum class PrintState { Standby, Printing, Paused, Complete, Cancelled, Error };
  struct Call {
    int conn = -1;  // -1 for HTTP
    std::string id; // the request's id as JSON text; empty for a notification
  };
  struct Conn {
    std::map<std::string, std::vector<std::string>> subs;  // object -> fields (empty: all)
    std::map<std::string, std::string> last;               // "object.field" -> JSON sent
  };
  struct HistoryJob {
    std::string job_id, filename, status;
    double start_time = 0, end_time = 0, total_duration = 0, print_duration = 0;
    // The slicer's filament total once the job completed; unknown (0) for one that didn't.
    double filament_used = 0;
    // The file's metadata when the job began, as Moonraker keeps it: the file may change or
    // go later. Mainsail's History shows its thumbnail, estimate and filament.
    bool has_meta = false;
    Metadata meta;
    uint32_t size = 0;
    double modified = 0;
  };
  // A handler returns 0 with `result` set, an error code with `err`, or kDeferred when it
  // will answer later through reply().
  static const int kDeferred = 1;
  using Handler = int (Service::*)(const Call&, JsonVariantConst, JsonDocument&, std::string&);

  int dispatch(const Call& c, const std::string& method, JsonVariantConst params,
               JsonDocument& result, std::string& err);
  std::string rpc(int conn, JsonVariantConst req);
  void reply(const Call& c, int code, const JsonDocument& result, const std::string& err);
  void notify(const std::string& method, JsonVariantConst params, int only_conn = -1);
  void notify_all(const std::string& json);

  // Handlers, one per method.
  int m_identify(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_server_info(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_server_config(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_printer_info(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_system_info(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_proc_stats(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_update_status(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_update_client(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_update_refresh(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_update_full(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int start_update(const std::string& name, JsonDocument& r, std::string& err);
  void update_status(JsonObject r);
  int m_webcams(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_announcements(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_objects_list(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_objects_query(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_objects_subscribe(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_gcode_script(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_gcode_help(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_gcode_store(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_temperature_store(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_print_start(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_print_pause(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_print_resume(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_print_cancel(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_emergency_stop(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_restart(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_query_endstops(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_machine_reboot(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_list(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_directory(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_metadata(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_delete(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_move(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_zip(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_files_no_folders(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_db_list(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_db_get(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_db_post(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_db_delete(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_history_list(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_history_totals(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_card_list(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_card_print(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_card_delete(const Call&, JsonVariantConst, JsonDocument&, std::string&);
  int m_card_copy(const Call&, JsonVariantConst, JsonDocument&, std::string&);

  // Printer objects.
  std::vector<std::string> object_names() const;
  bool build_object(const std::string& name, JsonObject out);
  void objects_status(JsonVariantConst objects, JsonObject status, Conn* remember);
  void push_status_updates();

  // The console.
  struct Script;
  bool run_line(const std::shared_ptr<Script>& s, const std::string& line, std::string& err);
  void script_line_done(const std::shared_ptr<Script>& s, const std::string& err);
  // Sends one line for a script. `check` sees the reply and may set its error; it can send
  // more lines for the same script.
  using Check = std::function<void(const Reply&, std::string& err)>;
  bool script_send(const std::shared_ptr<Script>& s, const std::string& gcode, std::string& err,
                   Check check = nullptr);

  // Calibration macros, in Mainsail's macro panel: PID autotune (M303) and the probe's Z
  // offset, which uses Mainsail's manual-probe dialog (TESTZ, ACCEPT, ABORT). Results are
  // in use at once and kept by SAVE_CONFIG (M500), as Klipper's are.
  bool calibration_allowed(std::string& err) const;
  bool pid_tune(const std::shared_ptr<Script>& s, const std::string& heater, double target,
                std::string& err);
  bool z_calibrate(const std::shared_ptr<Script>& s, std::string& err);
  bool z_test(const std::shared_ptr<Script>& s, const std::string& arg, std::string& err);
  bool z_finish(const std::shared_ptr<Script>& s, bool accept, std::string& err);
  void z_restore(const std::shared_ptr<Script>& s);
  bool has_probe() const { return link_.caps().has("Z_PROBE"); }
  std::string keep_until_saved(const std::string& item, const std::string& value);
  bool babystepping() const { return link_.caps().has("BABYSTEPPING"); }
  bool command_available(const std::string& name) const;  // for Mainsail's command list

  // The toolhead panel. Marlin reports neither which axes are homed nor its position
  // unasked, so Halyard follows G28 and asks M114 while the printer is idle.
  bool job_running() const;       // printing or paused, or a hand-off under way
  std::string homed_axes() const;
  void steppers_off(const std::string& gcode);  // M18/M84, as Marlin took it
  bool moves_homed(const std::string& script, std::string& err) const;
  bool z_offset_adjust(const std::shared_ptr<Script>& s, const std::vector<std::string>& words,
                       std::string& err);
  bool z_offset_apply_probe(const std::shared_ptr<Script>& s, std::string& err);
  bool pressure_advance(const std::shared_ptr<Script>& s, const std::vector<std::string>& words,
                        std::string& err);
  void ask_advance();
  bool velocity_limit(const std::shared_ptr<Script>& s, const std::vector<std::string>& words,
                      std::string& err);
  void ask_limits();
  void ask_factors();
  void store_gcode(const std::string& message, bool command);

  // The printer's card. M20 lists it only while the printer is idle; during a print the
  // last listing is shown. Printing or deleting a file on it waits for idle too.
  bool card_ready(std::string& why) const;
  bool card_reachable(const std::string& name, std::string& why) const;
  void card_list();
  void card_json(JsonObject r) const;
  const FileEntry* card_file(const std::string& name) const;

  // Jobs.
  void track_job();
  void begin_job(const std::string& filename);
  void end_job(PrintState how, const std::string& message);
  void history_changed(const char* action, const HistoryJob& j);
  void history_json(const HistoryJob& j, JsonObject o);
  void history_meta(HistoryJob& h);
  std::string library_name_for(const std::string& printer_file);
  void file_changed(const char* action, const std::string& name, const std::string& source);
  bool file_info(const std::string& name, JsonObject o, bool with_path);
  bool config_info(JsonObject o, bool with_path);
  // marlin.cfg: M503's report, read on connecting, after a print and after the console
  // changes a setting. Listed only once read; never written.
  void ask_settings();
  bool marlin_info(JsonObject o, bool with_path) const;
  std::string marlin_text() const;
  bool save_config(const std::string& text, std::string& err);
  void screen_poll();
  static void metadata_json(const Metadata& m, JsonObject o);

  // Mainsail's database.
  JsonVariant db_lookup(const std::string& ns, const std::string& key, bool create);
  void db_load();
  void db_save();

  double uptime() const { return now_ / 1000.0; }
  double wall() const;

  Link& link_;
  Printer& printer_;
  Delivery& delivery_;
  Library& lib_;
  Platform platform_;
  std::map<std::string, Handler> methods_;
  std::map<int, Conn> conns_;
  std::deque<std::pair<int, std::string>> outbox_;
  std::set<std::string> uploading_;  // names with an upload in progress
  // The last server.files.zip: its destination ("config/NAME.zip") and the items it names
  // ("gcodes/NAME" or "config/halyard.cfg"). The archive exists only as a download.
  std::string zip_path_;
  std::vector<std::string> zip_items_;
  uint32_t now_ = 0, last_status_push_ = 0, last_temp_sample_ = 0, last_proc_stat_ = 0;
  float cpu_temp_ = 0;
  bool cpu_temp_known_ = false;
  uint32_t reboot_at_ = 0;  // machine.reboot: after its reply has gone out
  std::vector<std::string> config_warnings_;
  std::string ui_check_error_;
  std::string release_, release_error_;  // the newest firmware release, from the last check
  bool update_busy_ = false;
  std::string screen_text_, screen_shown_;  // shown_: what the printer has, "" after a reconnect

  // Klippy state, as Mainsail sees it.
  std::string last_klippy_;
  bool volume_asked_ = false;
  double axis_min_[3] = {0, 0, 0}, axis_max_[3] = {200, 200, 200};

  // Since the printer connected: the axes Marlin homed (G28 seen, or a print), its last
  // M114 position, and the Z babysteps (M290) that gcode_move.homing_origin shows.
  std::string homed_;
  // Marlin's idle timeout (0: never), and the last command that moved the steppers.
  uint32_t stepper_timeout_ms_ = 120000, last_motion_ = 0;
  int heat_waits_ = 0;  // M109/M190 sent, not yet answered
  double pos_[4] = {0, 0, 0, 0};
  uint32_t last_pos_poll_ = 0;
  bool pos_poll_out_ = false;
  double z_adjust_ = 0;
  // Linear Advance (M900), Marlin's pressure advance: found by asking, since M115 has no
  // capability for it. advance_start_ is K as the printer connected, Mainsail's default.
  bool lin_advance_ = false, advance_query_out_ = false;
  double advance_k_ = 0, advance_start_ = 0;
  // The Limits panel: M203's X/Y feedrate and M204's printing acceleration, from their
  // reports (asked without parameters). *_start_ are the values as the printer connected,
  // Mainsail's defaults. axis_accel_ is M201's X/Y maximum, which holds M204 down (0 unknown).
  bool limits_known_ = false;
  double velocity_ = 0, accel_ = 0, velocity_start_ = 0, accel_start_ = 0, axis_accel_ = 0;
  double want_velocity_ = 0, want_accel_ = 0;  // just set: to say why Marlin kept less
  // The Endstops panel: M119's last report, Marlin's name ("x_min", "z_probe") -> "open" or
  // "TRIGGERED". Over HTTP, which is answered at once, this is what the call returns.
  std::map<std::string, std::string> endstops_;
  // The speed and flow sliders: M220's and M221's percentages as Klipper's factors (1 is
  // 100 %), read from their reports. The printer's own Tune menu changes them too.
  double speed_factor_ = 1, extrude_factor_ = 1;
  bool factors_query_out_ = false;
  uint32_t last_factors_poll_ = 0;
  // marlin.cfg: M503's report (without "echo:"), when it was read, and the commands it
  // listed: one of those from the console means a setting changed, so it is read again.
  std::string settings_report_;
  double settings_time_ = 0;
  std::set<std::string> settings_codes_;
  bool settings_stale_ = false, settings_query_out_ = false;

  // The printer's card, as M20 last listed it. card_version_ changes with every listing, so
  // a panel showing it knows to ask again (the halyard object carries it).
  std::vector<FileEntry> card_files_;
  bool card_listed_ = false, card_list_out_ = false, card_stale_ = true;
  std::string card_error_;  // why the last listing failed
  double card_listed_at_ = 0;
  uint32_t card_version_ = 0;
  std::vector<Call> card_waiting_;  // printer.card.list calls waiting for M20

  // The current job.
  PrintState pstate_ = PrintState::Standby;
  std::string job_file_, message_;
  Job last_job_ = Job::Unknown;
  uint32_t last_prints_done_ = 0;
  bool handoff_active_ = false, handoff_print_ = false, cancel_requested_ = false;
  double job_start_ = 0, print_duration_ = 0;
  // A finished job keeps its numbers on show until SDCARD_RESET_FILE, as Klipper's do.
  double final_total_ = 0, final_progress_ = 0, final_filament_ = 0;
  uint32_t last_track_ = 0;
  std::deque<HistoryJob> history_;
  int next_job_id_ = 1;
  bool job_in_history_ = false;
  struct Totals {
    int jobs = 0;
    double time = 0, print_time = 0, filament = 0, longest_job = 0, longest_print = 0;
  } totals_;

  // Console.
  int scripts_running_ = 0;  // while > 0, printer lines reach the console as script replies
  struct GcodeLog {
    std::string message;
    double time;
    bool command;
  };
  std::deque<GcodeLog> gcode_store_;

  // Calibration.
  bool pid_running_ = false;
  struct ManualProbe {
    bool starting = false, active = false;
    double z = 0;       // the nozzle's Z, in Marlin's coordinates
    double ref = 0;     // G30's measurement of the same spot
    double offset = 0;  // M851 Z before calibrating
    bool soft_endstops = false, leveling = false;  // to switch on again afterwards
    std::vector<double> tried;                     // sorted Z positions tested
  } probe_;
  std::map<std::string, std::string> save_pending_;  // section -> what SAVE_CONFIG keeps

  // Temperature history, one sample a second.
  static const size_t kTempSamples = 1200;
  std::vector<float> temps_[4];  // hotend, hotend target, bed, bed target
  std::vector<uint8_t> powers_[2];  // hotend, bed: percent
  size_t temp_head_ = 0, temp_count_ = 0;

  JsonDocument db_;
  bool db_dirty_ = false;
  uint32_t db_changed_ = 0;
};

}  // namespace halyard
