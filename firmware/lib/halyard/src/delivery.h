// SPDX-License-Identifier: GPL-3.0-or-later
// The delivery layer: hands a file from Halyard's library to the printer's own card and,
// if asked, starts it there. Only on command: nothing is handed off or started by itself,
// because the bed may still hold the last print (docs/decisions.md D9). Portable C++.
//
// A hand-off:
//   1. Check the printer is idle; M28 would abort a running media print.
//   2. List the printer's card (M20). Pick the file's 8.3 name, or reuse the one it had.
//      If the card already holds this exact file (same name and size as recorded), skip 3.
//   3. With print: preheat first (M140/M104, not waiting), so the heaters warm during the
//      transfer. Then M28, stream, M29, with the size checked by M20 (Printer::upload).
//   4. With print: M23 + M24.
// If anything fails after the preheat, the heaters are switched off again.
#pragma once
#include <functional>
#include <string>

#include "library.h"
#include "printer.h"

namespace halyard {

class Delivery {
 public:
  using Done = Printer::Done;
  Delivery(Printer& printer, Library& library) : printer_(printer), lib_(library) {}

  // Starts a hand-off of library file `name`; with `print` it preheats and starts the print.
  // Returns false with `err` set if it cannot start now; `done` is then not called.
  bool start(const std::string& name, bool print, Done done, std::string& err);
  void abort();
  void poll(uint32_t now_ms);  // call after Printer::poll

  bool busy() const { return step_ != Step::Idle; }
  struct Progress {
    std::string name, printer_name;
    const char* step = "idle";
    uint32_t bytes = 0, total = 0;  // file bytes read so far, of the file on Halyard's card
    uint32_t upload_ms = 0;         // time spent writing to the printer's card so far
  };
  Progress progress() const;

  std::function<void(const std::string&)> on_event;

 private:
  enum class Step { Idle, List, Preheat, Upload, Uploading, Start, Starting };

  void event(const std::string& s);
  void fail(const std::string& msg);
  void finish(bool ok, const std::string& msg);

  Printer& printer_;
  Library& lib_;
  Step step_ = Step::Idle;
  uint32_t now_ = 0, upload_start_ = 0;
  bool print_ = false, heated_ = false, aborted_ = false, waiting_ = false, cached_ = false;
  LibraryEntry entry_;
  std::string printer_name_;
  FileLineSource* src_ = nullptr;  // owned by the upload while it runs
  Done done_;
};

}  // namespace halyard
