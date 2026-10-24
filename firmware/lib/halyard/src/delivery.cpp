// SPDX-License-Identifier: GPL-3.0-or-later
#include "delivery.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <strings.h>

namespace halyard {

namespace {

// Preheat only to temperatures a slicer could plausibly mean; Marlin enforces its own
// limits too, this just refuses to act on a garbled value.
constexpr double kMaxHotend = 300, kMaxBed = 130;

std::string upper(std::string s) {
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::string temp_str(double t) {
  char buf[16];
  snprintf(buf, sizeof buf, "%d", (int)(t + 0.5));
  return buf;
}

}  // namespace

void Delivery::event(const std::string& s) {
  if (on_event) on_event(s);
}

bool Delivery::start(const std::string& name, bool print, Done done, std::string& err) {
  if (busy()) return err = "a hand-off is already running", false;
  if (printer_.uploading()) return err = "an upload to the printer is running", false;
  const Status& s = printer_.status();
  if (s.job == Job::Unknown) return err = "printer state not known yet", false;
  if (s.job != Job::Idle)
    return err = std::string("printer is ") + job_name(s.job) + "; hand-off waits for idle", false;
  if (!lib_.get(name, entry_)) return err = "no such file on Halyard: " + name, false;
  print_ = print;
  done_ = std::move(done);
  heated_ = aborted_ = waiting_ = cached_ = false;
  printer_name_.clear();
  src_ = nullptr;
  step_ = Step::List;
  return true;
}

void Delivery::abort() {
  if (!busy()) return;
  aborted_ = true;
  if (step_ == Step::Uploading) printer_.abort_upload();
}

Delivery::Progress Delivery::progress() const {
  Progress p;
  p.name = entry_.name;
  p.printer_name = printer_name_;
  switch (step_) {
    case Step::Idle: p.step = "idle"; break;
    case Step::List: p.step = "listing"; break;
    case Step::Preheat: p.step = "preheating"; break;
    case Step::Upload:
    case Step::Uploading: p.step = "uploading"; break;
    case Step::Start:
    case Step::Starting: p.step = "starting"; break;
  }
  p.total = entry_.size;
  if (src_) p.bytes = src_->bytes_read();
  else if (step_ == Step::Start || step_ == Step::Starting) p.bytes = p.total;
  if (step_ == Step::Uploading) p.upload_ms = now_ - upload_start_;
  return p;
}

void Delivery::finish(bool ok, const std::string& msg) {
  step_ = Step::Idle;
  src_ = nullptr;
  Done d = std::move(done_);
  done_ = nullptr;
  if (d) d(ok, msg);
}

void Delivery::fail(const std::string& msg) {
  std::string m = msg;
  if (heated_) {
    // Don't leave heaters on for a print that is not going to happen.
    bool off = printer_.send_raw("M104 S0", nullptr) && printer_.send_raw("M140 S0", nullptr);
    m += off ? "; heaters switched off" : "; COULD NOT SWITCH HEATERS OFF";
    heated_ = false;
  }
  finish(false, m);
}

void Delivery::poll(uint32_t now_ms) {
  now_ = now_ms;
  if (step_ == Step::Idle || waiting_) return;
  if (aborted_ && step_ != Step::Uploading && step_ != Step::Starting) return fail("aborted");

  switch (step_) {
    case Step::List:
      waiting_ = true;
      printer_.list_files([this](bool ok, const std::vector<FileEntry>& files) {
        waiting_ = false;
        if (step_ != Step::List) return;
        if (!ok) return fail("could not list the printer's card");
        std::vector<std::string> taken = lib_.printer_names(entry_.name);
        const FileEntry* mine = nullptr;
        for (auto& f : files) {
          if (!entry_.printer_name.empty() &&
              strcasecmp(f.name.c_str(), entry_.printer_name.c_str()) == 0)
            mine = &f;
          else
            taken.push_back(upper(f.name));
        }
        // Reuse the file's own name unless another library file has claimed it since.
        bool reuse = !entry_.printer_name.empty() &&
                     std::find(taken.begin(), taken.end(), upper(entry_.printer_name)) == taken.end();
        printer_name_ = reuse ? upper(entry_.printer_name) : short_name(entry_.name, taken);
        if (printer_name_.empty()) return fail("no free 8.3 name on the printer's card");
        cached_ = reuse && mine && entry_.printer_size && mine->size == entry_.printer_size;
        if (cached_) event("hand-off: " + printer_name_ + " is already on the printer's card");
        step_ = print_ ? Step::Preheat : cached_ ? Step::Start : Step::Upload;
      });
      return;

    case Step::Preheat: {
      const Metadata& m = entry_.meta;
      std::string what;
      if (m.first_layer_bed_temp > 0 && m.first_layer_bed_temp <= kMaxBed &&
          printer_.send_raw("M140 S" + temp_str(m.first_layer_bed_temp), nullptr)) {
        heated_ = true;
        what += " bed " + temp_str(m.first_layer_bed_temp);
      }
      if (m.first_layer_extr_temp > 0 && m.first_layer_extr_temp <= kMaxHotend &&
          printer_.send_raw("M104 S" + temp_str(m.first_layer_extr_temp), nullptr)) {
        heated_ = true;
        what += " hotend " + temp_str(m.first_layer_extr_temp);
      }
      event(what.empty() ? "hand-off: no preheat (no temperatures in the file)"
                         : "hand-off: preheating" + what);
      step_ = cached_ ? Step::Start : Step::Upload;
      return;
    }

    case Step::Upload: {
      std::unique_ptr<FileLineSource> src = lib_.open_lines(entry_.name);
      if (!src) return fail("cannot read " + entry_.name + " on Halyard's card");
      // Claim the name before writing, so a reset mid-transfer can't hand it to another file.
      if (!lib_.set_printer_file(entry_.name, printer_name_, 0))
        return fail("cannot update Halyard's metadata");
      FileLineSource* raw = src.get();
      bool ok = printer_.upload(printer_name_, std::move(src), [this](bool ok, const std::string& msg) {
        src_ = nullptr;
        if (step_ != Step::Uploading) return;
        if (!ok) return fail("upload failed: " + msg);
        lib_.set_printer_file(entry_.name, printer_name_, printer_.last_upload_bytes());
        event("hand-off: " + entry_.name + " -> " + printer_name_ + ", " + msg);
        if (!print_) return finish(true, "on the printer's card as " + printer_name_);
        step_ = Step::Start;
      });
      if (!ok) return fail("printer refused the upload (busy or printing)");
      src_ = raw;
      step_ = Step::Uploading;
      upload_start_ = now_;
      event("hand-off: writing " + entry_.name + " to the printer's card as " + printer_name_);
      return;
    }

    case Step::Start:
      if (!print_) return finish(true, "on the printer's card as " + printer_name_);
      step_ = Step::Starting;
      printer_.start_print(printer_name_, [this](bool ok, const std::string& msg) {
        if (step_ != Step::Starting) return;
        if (!ok) return fail("start failed: " + msg);
        heated_ = false;  // the print owns the heaters now
        finish(true, "printing " + entry_.name + " (" + printer_name_ + ")");
      });
      return;

    case Step::Idle:
    case Step::Uploading:
    case Step::Starting:
      return;
  }
}

}  // namespace halyard
