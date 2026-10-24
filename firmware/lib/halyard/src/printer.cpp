// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "binary_transfer.h"
#include "gcode_line.h"

namespace halyard {

namespace {

constexpr uint32_t kPollMs = 2000;
constexpr uint32_t kCancelTimeoutMs = 60000;
// How recently the printer must have said it is idle for Halyard to reconnect on its own
// when it stops answering: the link gives up after 2 x 5 s, plus one poll interval.
constexpr uint32_t kIdleFreshMs = 15000;
constexpr size_t kUploadQueueDepth = 8;  // lines handed to the link ahead of the window
constexpr int kTries = 3;

bool has_line(const Reply& r, const char* text) {
  for (auto& l : r.lines)
    if (l.find(text) != std::string::npos) return true;
  return false;
}

std::string first_line(const Reply& r) {
  return r.lines.empty() ? std::string(outcome_name(r.outcome)) : r.lines.front();
}

bool accepted(const Reply& r) {
  return r.outcome == Outcome::Ok || r.outcome == Outcome::OkInferred;
}

std::string trim_left(const std::string& s) {
  size_t i = s.find_first_not_of(' ');
  return i == std::string::npos ? std::string() : s.substr(i);
}

}  // namespace

const char* job_name(Job j) {
  switch (j) {
    case Job::Unknown: return "unknown";
    case Job::Idle: return "idle";
    case Job::Printing: return "printing";
    case Job::Paused: return "paused";
    case Job::Cancelling: return "cancelling";
  }
  return "?";
}

bool TestJobSource::next(std::string& line) {
  // Two lines per second of printing, plus a comment to exercise the stripping.
  int total = seconds_;
  if (i_ == 0) {
    line = "; Halyard test job: M117 and G4 only, nothing heats or moves";
  } else if (i_ <= 2 * total) {
    int s = (i_ - 1) / 2;
    line = (i_ % 2) ? "M117 Halyard test " + std::to_string(s + 1) + "/" + std::to_string(total)
                    : "G4 S1";
  } else if (i_ == 2 * total + 1) {
    line = "M117 Halyard test done";
  } else {
    return false;
  }
  i_++;
  return true;
}

// ---- upload state -----------------------------------------------------------------------

struct Printer::Upload {
  enum Step { CheckIdle, Remount, WaitLinkIdle, Open, Stream, Drain, Close, BinEnter, BinStream,
              Settle, Verify };
  Step step = CheckIdle;
  std::string name;
  std::unique_ptr<LineSource> src;
  Done done;
  bool waiting = false;     // this step's command is on its way
  int tries = 0;            // Marlin's replies carry no checksum; noise can garble them
  bool source_done = false;
  bool failed = false;      // stop, close the file, delete it
  std::string fail_msg;
  size_t line_no = 0;       // lines read from the source, for error messages
  size_t lines = 0, bytes = 0, lost = 0;
  uint32_t started = 0, last_report = 0;
  uint32_t resends_at_start = 0;
  std::unique_ptr<BinaryTransfer> bin;  // the binary transfer, while it runs
  std::string mode = "ascii";
  std::string bin_summary;              // how the binary transfer went, for the result
  uint32_t settle_until = 0;
  Step after_remount = WaitLinkIdle;
  bool finish_after_remount = false;  // the binary transfer failed; remount, then report
  bool delete_partial = false;        // ... and delete what it left, by name (M30)

  void fail(const std::string& msg) {
    if (!failed) fail_msg = msg;
    failed = true;
  }
};

Printer::~Printer() = default;

Printer::Printer(Link& link) : link_(link) {
  link_.on_line = [this](const std::string& l) { on_link_line(l); };
  link_.on_state = [this](LinkState s) { on_link_state(s); };
  link_.on_gave_up = [this](const std::string& cmd, bool silent) { on_gave_up(cmd, silent); };
}

// A printer that stops answering has either restarted at another baud rate (reflashed
// while Halyard stayed powered) or is busy without saying so. A handshake tries other baud
// rates, which is noise to a printer that is printing, so reconnect only when it said it
// was idle just before it went quiet, and nothing readable has come back since.
void Printer::on_gave_up(const std::string& cmd, bool silent) {
  bool was_responding = status_.responding;
  status_.responding = false;
  bool idle_fresh = status_.job == Job::Idle && idle_seen_valid_ && now_ - idle_seen_ <= kIdleFreshMs;
  if (silent && idle_fresh && !up_) {
    event("printer not answering (" + cmd + ") and was idle; reconnecting");
    reconnect_due_ = true;
    return;
  }
  if (was_responding)
    event("printer not answering (" + cmd + "); not reconnecting on my own while it may be "
          "printing. Use reconnect when it is safe");
}

void Printer::event(const std::string& s) {
  if (on_event) on_event(s);
}

void Printer::set_job(Job j) {
  if (j == status_.job) return;
  bool was_active = status_.job == Job::Printing || status_.job == Job::Paused ||
                    status_.job == Job::Cancelling;
  status_.job = j;
  paused_pos_valid_ = false;
  event(std::string("job: ") + job_name(j));
  if (j == Job::Idle && was_active && sd_autoreport_on_) {
    link_.send("M27 S0");
    sd_autoreport_on_ = false;
  }
  if (j == Job::Idle && cancel_done_) {
    Done d = std::move(cancel_done_);
    cancel_done_ = nullptr;
    d(true, "cancelled");
  }
}

// ---- what the printer says --------------------------------------------------------------

void Printer::parse_temps(const std::string& line) {
  // " T:24.2 /0.0 B:23.4 /0.0 @:0 B@:0", or the same after "ok " for M105.
  const char* t = strstr(line.c_str(), "T:");
  if (!t) return;
  float a, b;
  if (sscanf(t, "T:%f /%f", &a, &b) == 2) {
    status_.hotend = a;
    status_.hotend_target = b;
    status_.temps_valid = true;
  }
  const char* bed = strstr(line.c_str(), " B:");
  if (bed && sscanf(bed, " B:%f /%f", &a, &b) == 2) {
    status_.bed = a;
    status_.bed_target = b;
  }
  // The heaters' soft PWM, 0 to 127 (Temperature::getHeaterPower()).
  auto power = [](float raw) { return std::min(1.0f, std::max(0.0f, raw / 127)); };
  const char* p = strstr(line.c_str(), " @:");
  if (p && sscanf(p, " @:%f", &a) == 1) status_.hotend_power = power(a);
  p = strstr(line.c_str(), " B@:");
  if (p && sscanf(p, " B@:%f", &a) == 1) status_.bed_power = power(a);
}

void Printer::on_link_line(const std::string& line) {
  if (on_line) on_line(line);
  std::string t = trim_left(line);

  if (starts_with(t, "T:") || starts_with(t, "ok T:")) {
    parse_temps(t);
  } else if (starts_with(t, "SD printing byte ")) {
    unsigned long pos = 0, size = 0;
    if (sscanf(t.c_str() + 17, "%lu/%lu", &pos, &size) == 2) {
      bool changed = pos != status_.sd_pos;
      status_.sd_pos = pos;
      status_.sd_size = size;
      if (status_.job == Job::Unknown || status_.job == Job::Idle) set_job(Job::Printing);
      // Paused, Marlin still says "SD printing byte", at a position that stays put. One
      // that moves was resumed on the printer's own screen (or after its M600).
      if (status_.job == Job::Paused) {
        if (!paused_pos_valid_) paused_pos_ = pos, paused_pos_valid_ = true;
        else if (pos != paused_pos_) set_job(Job::Printing);
      }
      if (changed && size)
        event("progress " + std::to_string(pos) + "/" + std::to_string(size) + " (" +
              std::to_string(pos * 1000 / size / 10) + "." + std::to_string(pos * 1000 / size % 10) +
              "%)");
    }
  } else if (t == "Not SD printing") {
    idle_seen_ = now_;
    idle_seen_valid_ = true;
    set_job(Job::Idle);
  } else if (starts_with(t, "Done printing file")) {
    status_.prints_done++;
    event("print finished");
    set_job(Job::Idle);
  } else if (starts_with(t, "Current file: ")) {
    status_.file = t.substr(14);
  } else if (starts_with(t, "File opened: ")) {
    size_t sz = t.find(" Size: ");
    status_.file = t.substr(13, sz == std::string::npos ? sz : sz - 13);
    if (sz != std::string::npos) status_.sd_size = strtoul(t.c_str() + sz + 7, nullptr, 10);
    status_.sd_pos = 0;
  } else if (starts_with(t, "//action:")) {
    // HOST_ACTION_COMMANDS: M25 says "pause" and M24 "resume" (including M24 from the
    // printer's own menu, or at a print's start), M600 "paused" and "resumed". The only way
    // to hear of a pause from the printer's screen: M27 reports a paused print as printing.
    std::string a = t.substr(9);
    event("host action: " + a);
    if ((a == "pause" || a == "paused") && status_.job == Job::Printing) set_job(Job::Paused);
    else if ((a == "resume" || a == "resumed") && status_.job == Job::Paused) set_job(Job::Printing);
  }
}

void Printer::on_link_state(LinkState s) {
  if (on_state) on_state(s);
  status_.responding = true;
  idle_seen_valid_ = false;
  if (s == LinkState::Ready) return;
  setup_done_ = false;
  binary_broken_ = false;
  temp_poll_out_ = sd_poll_out_ = false;
  sd_autoreport_on_ = false;
  status_.temps_valid = false;
  status_.job = Job::Unknown;
  if (up_) upload_finish(false, "link lost during upload");
  if (cancel_done_) {
    Done d = std::move(cancel_done_);
    cancel_done_ = nullptr;
    d(false, "link lost during cancel");
  }
}

// ---- polling ----------------------------------------------------------------------------

void Printer::poll(uint32_t now) {
  now_ = now;
  if (reconnect_due_) {
    reconnect_due_ = false;
    link_.reconnect();
  }
  link_.poll(now);
  if (link_.state() != LinkState::Ready) return;

  const Caps& c = link_.caps();
  if (!setup_done_) {
    setup_done_ = true;
    if (c.has("AUTOREPORT_TEMP")) link_.send("M155 S2");
    link_.send("M27 C");
    link_.send("M27");
    last_temp_poll_ = last_sd_poll_ = now;
  }

  if (up_) {
    upload_poll();
    return;  // anything else sent now would be written into the file
  }

  // The handshake found Marlin stuck in binary mode and closed the transfer it had open
  // (the partial file stays, under Halyard's 8.3 name). That released the printer's card.
  if (link_.rescued_binary()) {
    link_.clear_rescued_binary();
    remount_needed_ = true;
    event("closed a binary transfer an earlier connection left open; its partial file stays");
  }

  // Each poll waits for its own answer, not for an idle link: with one line in flight and
  // the printer's queue full of media lines, a reply takes seconds, and polls that each
  // wait for idle starve one another.
  if (!c.has("AUTOREPORT_TEMP") && !temp_poll_out_ && now - last_temp_poll_ >= kPollMs) {
    temp_poll_out_ = link_.send("M105", [this](const Reply& r) {
      temp_poll_out_ = false;
      if (accepted(r)) status_.responding = true;
    });
    last_temp_poll_ = now;
  }

  // Polled while idle too: that notices a print started from the printer's own screen, and
  // keeps "idle" current for on_gave_up().
  bool pushed = sd_autoreport_on_ && status_.job != Job::Cancelling;
  if (!pushed && !sd_poll_out_ && now - last_sd_poll_ >= kPollMs) {
    sd_poll_out_ = link_.send("M27", [this](const Reply& r) {
      sd_poll_out_ = false;
      if (accepted(r)) status_.responding = true;
    });
    last_sd_poll_ = now;
  }
  if (status_.job == Job::Printing && c.has("AUTOREPORT_SD_STATUS") && !sd_autoreport_on_) {
    link_.send("M27 S2");
    sd_autoreport_on_ = true;
  }
}

// ---- file list --------------------------------------------------------------------------

void Printer::list_files(std::function<void(bool, const std::vector<FileEntry>&)> done) {
  // T is ignored by firmware without M20_TIMESTAMP_SUPPORT, so it is always safe to ask.
  std::string cmd = link_.caps().has("LONG_FILENAME") ? "M20 L T" : "M20 T";
  bool want_long = link_.caps().has("LONG_FILENAME");
  bool ok = link_.send(cmd, [this, done, want_long](const Reply& r) {
    // "No media" while idle: a binary transfer released the card (card.release()) and
    // nobody remounted it, e.g. Halyard restarted in between. Mount it and list again.
    // Never while a print may be running: M21 re-initialises the card under it.
    if (has_line(r, "No media") && status_.job == Job::Idle && !remounting_) {
      remounting_ = true;
      bool sent = link_.send("M21", [this, done](const Reply& m) {
        if (!has_line(m, "SD card ok")) {
          remounting_ = false;
          return done(false, {});
        }
        remount_needed_ = false;
        list_files([this, done](bool ok2, const std::vector<FileEntry>& f) {
          remounting_ = false;
          done(ok2, f);
        });
      });
      if (!sent) {
        remounting_ = false;
        done(false, {});
      }
      return;
    }
    std::vector<FileEntry> files;
    bool in_list = false, complete = false;
    for (auto& l : r.lines) {
      if (l == "Begin file list") { in_list = true; continue; }
      if (l == "End file list") { complete = true; break; }
      if (!in_list) continue;
      FileEntry f;
      size_t sp = l.find(' ');
      if (sp == std::string::npos) continue;
      f.name = l.substr(0, sp);
      char* rest = nullptr;
      f.size = strtoul(l.c_str() + sp + 1, &rest, 10);
      std::string tail = rest && *rest == ' ' ? std::string(rest + 1) : std::string();
      if (starts_with(tail, "0x")) {
        size_t e = tail.find(' ');
        f.timestamp = tail.substr(0, e);
        tail = e == std::string::npos ? std::string() : tail.substr(e + 1);
      }
      if (want_long) f.long_name = tail;
      files.push_back(f);
    }
    done(accepted(r) && complete, files);
  });
  if (!ok) done(false, {});
}

// ---- upload -----------------------------------------------------------------------------

bool Printer::upload(const std::string& name, std::unique_ptr<LineSource> src, Done done) {
  if (up_ || link_.state() != LinkState::Ready) return false;
  if (status_.job == Job::Printing || status_.job == Job::Paused) return false;
  up_.reset(new Upload());
  up_->name = name;
  up_->src = std::move(src);
  up_->done = std::move(done);
  up_->started = up_->last_report = now_;
  up_->resends_at_start = link_.stats().recoveries;
  return true;
}

void Printer::abort_upload() {
  if (up_) up_->fail("aborted");
}

void Printer::upload_finish(bool ok, const std::string& msg) {
  if (!up_) return;
  std::unique_ptr<Upload> u = std::move(up_);
  if (u->done) u->done(ok, msg);
}

void Printer::report_progress(Upload& u) {
  if (now_ - u.last_report < 2000) return;
  u.last_report = now_;
  uint32_t secs = (now_ - u.started) / 1000;
  event("upload: " + std::to_string(u.lines) + " lines, " + std::to_string(u.bytes) + " bytes" +
        (secs ? ", " + std::to_string(u.bytes / secs) + " B/s" : "") + " (" + u.mode + ")");
}

bool Printer::next_clean(Upload& u, std::string& clean) {
  std::string raw;
  while (!u.failed && !u.source_done) {
    if (!u.src->next(raw)) {
      u.source_done = true;
      return false;
    }
    u.line_no++;
    if (const char* why = sanitize_for_upload(raw, clean)) {
      u.fail("line " + std::to_string(u.line_no) + ": " + why);
      return false;
    }
    if (!clean.empty()) return true;
  }
  return false;
}

// The binary transfer, fed from the source. Ends in Verify, in an ASCII retry when the
// printer never answered in binary mode, or in failure.
void Printer::binary_poll(Upload& u) {
  BinaryTransfer& b = *u.bin;
  std::string clean;
  while (!u.failed && !u.source_done && b.wants_data() && next_clean(u, clean)) {
    // Marlin reads a printed line into its MAX_CMD_SIZE buffer and cuts it there; the
    // ASCII path refuses such lines too (with the N and checksum on top).
    if (clean.size() > kMaxWireLine) {
      u.fail("line " + std::to_string(u.line_no) + ": too long for Marlin's line buffer");
      break;
    }
    b.feed(clean + "\r\n");
    u.lines++;
    u.bytes += clean.size() + 2;
  }
  if (u.failed) b.abort(u.fail_msg);
  else if (u.source_done) b.end_of_data();
  b.poll(now_);
  report_progress(u);
  if (!b.finished()) return;

  const BinaryTransfer::Stats& st = b.stats();
  char ratio[16];
  snprintf(ratio, sizeof ratio, "%.2f", st.wire_payload ? (double)st.raw_bytes / st.wire_payload : 1.0);
  u.bin_summary = std::string("binary") + (b.compressed() ? ", compressed " + std::string(ratio) + "x" : "") +
                  ", " + std::to_string(st.packets) + " packets, " + std::to_string(st.resent) +
                  " resent (" + std::to_string(st.resend_requests) + " asked, " +
                  std::to_string(st.timeouts) + " timeouts), window " + std::to_string(st.window);
  bool ok = b.ok(), left = b.left_binary_mode();
  bool partial = b.partial_file();
  std::string err = b.error();
  bool never_synced = b.state() == BinaryTransfer::State::Failed && st.packets == 0;
  if (never_synced) link_.raw_write("\n");  // end the junk line the SYNC bytes made
  link_.exit_raw();
  u.bin.reset();
  if (ok) {
    u.step = Upload::Remount;
    u.after_remount = Upload::Verify;
    u.tries = 0;
    return;
  }
  if (never_synced && !u.failed) {
    // M28 B1 did not switch the printer over: it never answered a SYNC. Nothing has been
    // read from the source yet, so carry on in ASCII, once the answer to the junk line the
    // SYNC bytes made has come and gone.
    binary_broken_ = true;
    event("upload: printer did not answer in binary mode; using ASCII");
    u.mode = "ascii";
    u.bin_summary.clear();
    u.step = Upload::Settle;
    u.settle_until = now_ + 500;
    return;
  }
  std::string msg = u.failed ? u.fail_msg : "binary transfer: " + err;
  remount_needed_ = true;
  if (left) {
    u.fail_msg = msg;
    u.failed = true;
    u.finish_after_remount = true;
    u.delete_partial = partial;
    u.step = Upload::Remount;
    u.tries = 0;
    return;
  }
  upload_finish(false, msg + "; reconnecting");  // frees u
  // Marlin may still be in binary mode. The printer is idle (checked before M28), so a
  // handshake is safe, and it begins with the SYNC and CLOSE that bring Marlin back.
  if (!left) link_.reconnect();
}

void Printer::upload_poll() {
  Upload& u = *up_;
  switch (u.step) {
    case Upload::CheckIdle:
      if (u.waiting) return;
      u.waiting = true;
      // M28 closes whatever media print is running, so make sure there is none.
      link_.send("M27", [this](const Reply& r) {
        if (!up_) return;
        up_->waiting = false;
        if (accepted(r) && has_line(r, "Not SD printing")) {
          up_->step = remount_needed_ ? Upload::Remount : Upload::WaitLinkIdle;
          up_->after_remount = Upload::WaitLinkIdle;
          up_->tries = 0;
        } else if (has_line(r, "SD printing byte")) {
          upload_finish(false, "printer is printing; M28 now would abort that print");
        } else if (++up_->tries >= kTries) {
          upload_finish(false, "could not confirm the printer is idle (" + first_line(r) + ")");
        }
      });
      return;

    case Upload::WaitLinkIdle:
      // Nothing else may follow M28 until it is confirmed: if it failed, the file's lines
      // would run as commands.
      if (!link_.idle()) return;
      u.step = binary_enabled_ && !binary_broken_ && link_.caps().has("BINARY_FILE_TRANSFER")
                   ? Upload::BinEnter
                   : Upload::Open;
      return;

    case Upload::BinEnter:
      if (u.waiting) return;
      u.waiting = true;
      // M28 B1 switches the port to binary packets once its ok is out. Nothing may be sent
      // after it but packets, so the link is idle here and stays so until the transfer ends.
      link_.send("M28 B1", [this](const Reply& r) {
        if (!up_) return;
        Upload& v = *up_;
        v.waiting = false;
        if (r.outcome == Outcome::Error || has_line(r, "Unknown command")) {
          binary_broken_ = true;  // the capability is claimed but M28 B1 isn't understood
          event("upload: M28 B1 refused; using ASCII");
          v.step = Upload::WaitLinkIdle;
          return;
        }
        // Switched, or a garbled answer: the SYNC packet settles which.
        v.mode = "binary";
        v.bin.reset(new BinaryTransfer([this](const std::string& b) { link_.raw_write(b); }));
        link_.enter_raw([this](const std::string& l) {
          if (up_ && up_->bin) up_->bin->on_line(l, now_);
        });
        v.bin->start(v.name, now_);
        v.step = Upload::BinStream;
      });
      return;

    case Upload::BinStream:
      return binary_poll(u);

    case Upload::Remount:
      // Marlin's binary transfer ends with card.release(): the printer's card is unmounted
      // until M21. Only ever here, with the printer known to be idle; M21 re-initialises
      // the card, which would break a print running from it.
      if (u.waiting) return;
      u.waiting = true;
      link_.send("M21", [this](const Reply& r) {
        if (!up_) return;
        Upload& v = *up_;
        v.waiting = false;
        if (has_line(r, "SD card ok")) {
          remount_needed_ = false;
          if (v.delete_partial) {
            // By name: never Marlin's binary ABORT (see binary_transfer.h).
            link_.send("M30 " + v.name, [this](const Reply&) {
              if (up_) upload_finish(false, up_->fail_msg + "; partial file deleted");
            });
            v.delete_partial = false;
            return;
          }
          if (v.finish_after_remount) return upload_finish(!v.failed, v.fail_msg);
          v.step = v.after_remount;
          v.tries = 0;
          return;
        }
        if (++v.tries >= kTries) {
          remount_needed_ = true;
          upload_finish(false, (v.finish_after_remount ? v.fail_msg + "; " : std::string()) +
                                   "printer's card did not mount again (" + first_line(r) + ")");
        }
      });
      return;

    case Upload::Settle:
      if ((int32_t)(now_ - u.settle_until) >= 0 && link_.idle()) u.step = Upload::Open;
      return;

    case Upload::Open:
      if (u.waiting) return;
      u.waiting = true;
      link_.send("M28 " + u.name, [this](const Reply& r) {
        if (!up_) return;
        up_->waiting = false;
        if (accepted(r) && has_line(r, "Writing to file")) {
          up_->step = Upload::Stream;
          up_->tries = 0;
          event("upload: writing " + up_->name);
          return;
        }
        // Unless Marlin said it could not open the file, the file may be open and the
        // printer in saving mode with a garbled reply. M29 is harmless either way.
        link_.send("M29");
        if (has_line(r, "open failed") || ++up_->tries >= kTries)
          upload_finish(false, "M28 failed: " + first_line(r));
        else
          up_->step = Upload::WaitLinkIdle;
      });
      return;

    case Upload::Stream: {
      std::string clean;
      while (link_.queued() < kUploadQueueDepth && next_clean(u, clean)) {
        bool sent = link_.send(clean, [this](const Reply& r) {
          if (!up_) return;
          if (r.outcome == Outcome::Lost) up_->lost++;  // the size check decides
          else if (r.outcome == Outcome::Error) up_->fail("printer error: " + first_line(r));
          else if (r.outcome == Outcome::Disconnected) up_->fail("link lost");
        });
        if (!sent) {
          u.fail("line " + std::to_string(u.line_no) + ": too long for Marlin's line buffer");
          break;
        }
        u.lines++;
        u.bytes += clean.size() + 2;  // Marlin writes each line with "\r\n"
      }
      report_progress(u);
      if (u.failed) u.step = Upload::Drain;
      else if (u.source_done) u.step = Upload::Close;
      return;
    }

    case Upload::Drain:
      if (link_.idle()) u.step = Upload::Close;
      return;

    case Upload::Close:
      if (u.waiting) return;
      u.waiting = true;
      link_.send("M29", [this](const Reply& r) {
        if (!up_) return;
        up_->waiting = false;
        // An ok means M29 ran and saving has ended, whatever its text says; M20 checks
        // the file. Without an ok, send it again.
        if (!accepted(r)) {
          if (++up_->tries >= kTries) upload_finish(false, "M29 not confirmed (" + first_line(r) + ")");
          return;
        }
        up_->tries = 0;
        if (up_->failed) {
          link_.send("M30 " + up_->name);  // don't leave a partial file behind
          upload_finish(false, up_->fail_msg + "; partial file deleted");
        } else {
          up_->step = Upload::Verify;
        }
      });
      return;

    case Upload::Verify:
      if (u.waiting) return;
      u.waiting = true;
      list_files([this](bool ok, const std::vector<FileEntry>& files) {
        if (!up_) return;
        Upload& v = *up_;
        const FileEntry* f = nullptr;
        for (auto& e : files)
          if (strcasecmp(e.name.c_str(), v.name.c_str()) == 0) f = &e;
        uint32_t ms = now_ - v.started;
        std::string summary = std::to_string(v.lines) + " lines, " + std::to_string(v.bytes) +
                              " bytes in " + std::to_string(ms / 1000) + "." +
                              std::to_string(ms % 1000 / 100) + " s";
        if (ms) summary += " (" + std::to_string((uint64_t)v.bytes * 1000 / ms) + " B/s)";
        summary += ", " + std::to_string(link_.stats().recoveries - v.resends_at_start) +
                   " resends, " + std::to_string(v.lost) + " unconfirmed lines";
        v.waiting = false;
        if (!v.bin_summary.empty()) summary += ", " + v.bin_summary;
        if (ok && f && f->size == v.bytes) {
          last_upload_bytes_ = (uint32_t)v.bytes;
          return upload_finish(true, summary);
        }
        if (++v.tries < kTries) return;  // a garbled listing line; list again
        if (!ok) upload_finish(false, "uploaded, but M20 failed; " + summary);
        else if (!f) upload_finish(false, v.name + " is missing from M20; " + summary);
        else
          upload_finish(false, "size mismatch: printer has " + std::to_string(f->size) +
                                   ", expected " + std::to_string(v.bytes) + "; " + summary);
      });
      return;
  }
}

// ---- printing ---------------------------------------------------------------------------

void Printer::start_print(const std::string& name, Done done) {
  if (up_) return done(false, "an upload is running");
  if (status_.job == Job::Printing || status_.job == Job::Paused)
    return done(false, "already printing");
  bool ok = link_.send("M23 " + name, [this, done](const Reply& r) {
    if (!has_line(r, "File selected")) return done(false, "M23: " + first_line(r));
    bool sent = link_.send("M24", [this, done](const Reply& r2) {
      if (!accepted(r2)) return done(false, std::string("M24: ") + outcome_name(r2.outcome));
      set_job(Job::Printing);
      done(true, "printing " + status_.file);
    });
    if (!sent) done(false, "link not ready");
  });
  if (!ok) done(false, "link not ready");
}

void Printer::pause(Done done) {
  if (status_.job != Job::Printing) return done(false, "not printing");
  bool ok = link_.send("M25", [this, done](const Reply& r) {
    if (!accepted(r)) return done(false, outcome_name(r.outcome));
    set_job(Job::Paused);
    done(true, "paused");
  });
  if (!ok) done(false, "link not ready");
}

void Printer::resume(Done done) {
  if (status_.job != Job::Paused) return done(false, "not paused");
  bool ok = link_.send("M24", [this, done](const Reply& r) {
    if (!accepted(r)) return done(false, outcome_name(r.outcome));
    set_job(Job::Printing);
    done(true, "resumed");
  });
  if (!ok) done(false, "link not ready");
}

void Printer::cancel(Done done) {
  if (status_.job != Job::Printing && status_.job != Job::Paused)
    return done(false, "not printing");
  // With EMERGENCY_PARSER Marlin acts on M524 as it arrives; without it M524 waits behind
  // whatever is queued. Either way the abort clears Marlin's queue, so lines sent before
  // it may never be answered; the link copes with that. Done when M27 says so.
  bool ok = link_.caps().has("EMERGENCY_PARSER") ? link_.send_now("M524") : link_.send("M524");
  if (!ok) return done(false, "link not ready");
  cancel_done_ = std::move(done);
  set_job(Job::Cancelling);
  last_sd_poll_ = now_ - kPollMs;  // poll right away
}

bool Printer::remove(const std::string& name, Done done) {
  // M30 closes the open file before deleting (card.closefile()), whichever file it names:
  // during a media print that is the job's own file.
  if (up_ || status_.job != Job::Idle) return false;
  return link_.send("M30 " + name, [done](const Reply& r) {
    bool ok = has_line(r, "File deleted");
    // A bare ok says nothing; Marlin's own words when it has any.
    done(ok, ok || !r.lines.empty() || !accepted(r) ? first_line(r)
                                                     : "the printer didn't say it was deleted");
  });
}

bool Printer::send_raw(const std::string& cmd, ReplyFn done) {
  if (up_) return false;
  return link_.send(cmd, std::move(done));
}

}  // namespace halyard
