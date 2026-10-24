// SPDX-License-Identifier: GPL-3.0-or-later
// Printer operations on top of the link: status, file list, M28 upload, print, pause,
// cancel. Adapts to the capabilities M115 reported and falls back to polling without them
// (docs/decisions.md D5). Portable C++, no Arduino.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "link.h"

namespace halyard {

struct FileEntry {
  std::string name;       // the 8.3 path Marlin uses for M23/M30
  uint32_t size = 0;
  std::string long_name;  // with LONG_FILENAME (M20 L), else empty
  std::string timestamp;  // with EXTENDED_M20 (M20 T), FAT date+time as hex, else empty
};

// The lines of a file to upload, one at a time.
struct LineSource {
  virtual ~LineSource() = default;
  virtual bool next(std::string& line) = 0;  // false at the end
};

// A harmless job for tests: M117 messages and G4 dwells, nothing that heats or moves.
class TestJobSource : public LineSource {
 public:
  explicit TestJobSource(int seconds) : seconds_(seconds) {}
  bool next(std::string& line) override;

 private:
  int seconds_;
  int i_ = 0;
};

enum class Job { Unknown, Idle, Printing, Paused, Cancelling };
const char* job_name(Job j);

struct Status {
  bool temps_valid = false;
  float hotend = 0, hotend_target = 0, bed = 0, bed_target = 0;
  float hotend_power = 0, bed_power = 0;  // 0 to 1, from "@:" and "B@:"
  Job job = Job::Unknown;
  uint32_t sd_pos = 0, sd_size = 0;
  std::string file;  // as Marlin reports it (8.3 name, then long name if known)
  bool responding = true;  // false after a command went unanswered, until a poll is answered
  // Prints that ended with "Done printing file". A job that goes idle without one was
  // cancelled (from Halyard or the printer's own menu) or failed.
  uint32_t prints_done = 0;
};

class Printer {
 public:
  using Done = std::function<void(bool ok, const std::string& msg)>;

  explicit Printer(Link& link);
  ~Printer();
  void poll(uint32_t now_ms);

  const Status& status() const { return status_; }
  bool uploading() const { return up_ != nullptr; }
  // Bytes the last successful upload wrote to the printer's card, as M20 confirmed.
  uint32_t last_upload_bytes() const { return last_upload_bytes_; }

  void list_files(std::function<void(bool ok, const std::vector<FileEntry>&)> done);

  // Writes a file to the printer's card, then checks its size with M20. Uses Marlin's binary
  // transfer (M28 B1, compressed when offered) when M115 reports BINARY_FILE_TRANSFER, else
  // ASCII M28/M29 with numbered, checksummed lines. Both write the same bytes: each line
  // stripped of its comment, with "\r\n". Refuses while printing: M28 aborts a media print.
  bool upload(const std::string& name, std::unique_ptr<LineSource> src, Done done);
  void abort_upload();
  // Binary transfer on or off (on by default). It also turns itself off for the rest of the
  // connection if the printer claims the capability but never answers in binary mode.
  void set_binary(bool on) { binary_enabled_ = on; }
  bool binary_enabled() const { return binary_enabled_; }

  void start_print(const std::string& name, Done done);
  void pause(Done done);
  void resume(Done done);
  void cancel(Done done);
  bool remove(const std::string& name, Done done);  // M30; refused unless idle

  // A raw command from a person at the console. Refused while an upload is writing,
  // since it would land in the file.
  bool send_raw(const std::string& cmd, ReplyFn done);

  // Printer owns the link's on_line, on_state and on_gave_up; listen here instead.
  std::function<void(const std::string&)> on_line;   // every line from the printer
  std::function<void(LinkState)> on_state;           // link state changes
  std::function<void(const std::string&)> on_event;  // progress, finished, host actions

 private:
  struct Upload;

  void on_link_line(const std::string& line);
  void on_link_state(LinkState s);
  void parse_temps(const std::string& line);
  void event(const std::string& s);
  void set_job(Job j);
  void on_gave_up(const std::string& cmd, bool silent);
  void upload_poll();
  void upload_finish(bool ok, const std::string& msg);
  bool next_clean(Upload& u, std::string& clean);
  void report_progress(Upload& u);
  void binary_poll(Upload& u);

  Link& link_;
  Status status_;
  uint32_t now_ = 0;
  uint32_t last_temp_poll_ = 0, last_sd_poll_ = 0;
  bool setup_done_ = false;
  bool sd_autoreport_on_ = false;
  bool temp_poll_out_ = false, sd_poll_out_ = false;
  uint32_t last_upload_bytes_ = 0;
  bool binary_enabled_ = true;
  bool binary_broken_ = false;  // the printer did not answer in binary mode this connection
  bool remount_needed_ = false; // a binary transfer released the printer's card (M21 due)
  bool remounting_ = false;     // list_files() is mounting the card; don't loop
  uint32_t idle_seen_ = 0;       // when M27 last said "Not SD printing"
  bool idle_seen_valid_ = false;
  // The position M27 reported first after a pause. A later one that differs means the
  // printer's own screen resumed the print.
  uint32_t paused_pos_ = 0;
  bool paused_pos_valid_ = false;
  bool reconnect_due_ = false;
  std::unique_ptr<Upload> up_;
  Done cancel_done_;
};

}  // namespace halyard
