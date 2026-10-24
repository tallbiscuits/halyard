// SPDX-License-Identifier: GPL-3.0-or-later
// The web UI (Mainsail) on Halyard's card, served the way nginx serves it for Moonraker.
// Its folder is root; root.prev holds the one before the last install.
// The files are stored gzipped ("index.html.gz"), since the card is slower than the network,
// and go out with Content-Encoding: gzip. Files that do not shrink (images, fonts) are stored
// as they are.
//
// It arrives as one tar stream (./run.sh webui builds it) and is unpacked into a staging
// directory. Only a complete archive with an index.html replaces the UI that is there, so a
// failed upload leaves the old one working. Portable C++ (POSIX stdio).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace halyard {

class WebUi {
 public:
  explicit WebUi(const std::string& root) : root_(root) {}

  bool installed() const;  // an index.html (or .gz) is there
  // Mainsail's release has a .version file ("v2.19.0"); "" if there is none.
  std::string version() const;
  // An install keeps the UI it replaces, so a new Mainsail that does not work with Halyard
  // can be undone (Mainsail 2.19's update panel has no rollback; the update page does it).
  std::string previous_version() const;  // "" if there is no previous UI
  bool restore_previous(std::string& err);  // swaps the two

  // Called between files during long card work: removing the old UI (about 200 files) or a
  // leftover staging folder. On the ESP32 the web server runs this on core 0 and SD access
  // busy-waits, so without a pause here the task watchdog resets Halyard (found on hardware:
  // any install over an existing UI).
  std::function<void()> on_busy;

  // A request path ("/", "/assets/index-x.js", "/files"), resolved to a file.
  struct File {
    std::string path;          // on the card
    std::string content_type;
    bool gzip = false;         // send Content-Encoding: gzip
    bool immutable = false;    // hashed asset name: cache for a year
  };
  // False if there is nothing to serve. Paths without an extension that match no file get
  // index.html, for Mainsail's own routes (nginx's try_files ... /index.html).
  bool lookup(const std::string& url_path, File& out) const;

  // ---- installing -------------------------------------------------------------------------
  class Install {
   public:
    ~Install();  // an unfinished install is removed
    bool feed(const uint8_t* data, size_t len);  // false: stop, error() says why
    bool finish();                               // after the last byte: swap it in
    const std::string& error() const { return error_; }
    int files() const { return files_; }
    uint64_t bytes() const { return bytes_; }

   private:
    friend class WebUi;
    explicit Install(WebUi& ui);
    bool header();
    bool fail(const std::string& msg);

    WebUi& ui_;
    std::string staging_;
    uint8_t block_[512];
    size_t have_ = 0;        // bytes in block_
    uint64_t left_ = 0;      // data bytes of the current entry still to come (then padding
                             // to the block)
    FILE* out_ = nullptr;
    std::string out_path_;
    int zero_blocks_ = 0;
    bool done_ = false, failed_ = false, finished_ = false;
    int files_ = 0;
    uint64_t bytes_ = 0;
    std::string error_;
  };
  std::unique_ptr<Install> begin_install();

 private:
  std::string root_;
};

}  // namespace halyard
