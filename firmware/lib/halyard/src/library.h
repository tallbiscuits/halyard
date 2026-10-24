// SPDX-License-Identifier: GPL-3.0-or-later
// Halyard's own file library: G-code files on its SD card, each with the slicer metadata and
// thumbnails taken from it while it was written, and the name and size it has on the
// printer's card once handed off. Plain POSIX stdio and dirent, which ESP-IDF's VFS provides,
// so the same code runs on the host in tests. Portable C++, no Arduino.
//
// Layout under the root (Moonraker's, so thumbnails are served as they are):
//   <name>                     the file as uploaded
//   <name>.part                an upload in progress
//   .thumbs/<stem>-<w>x<h>.png thumbnails
//   .meta/<name>.meta          metadata and printer mapping, "key=value" lines
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "metadata.h"
#include "printer.h"

namespace halyard {

// A whole small file. write_file goes via a temporary file, so a reset mid-write leaves the
// old contents or the new.
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);

struct LibraryEntry {
  std::string name;
  uint32_t size = 0;
  Metadata meta;
  // Set by a hand-off: the 8.3 name on the printer's card and the size Marlin wrote there
  // (comments stripped). printer_size is 0 when the printer's copy is unknown or stale.
  std::string printer_name;
  uint32_t printer_size = 0;
};

// The lines of a library file, for Printer::upload. Comments longer than the buffer are cut
// short (the upload strips them anyway), so memory stays bounded whatever the file holds.
class FileLineSource : public LineSource {
 public:
  explicit FileLineSource(FILE* f, uint32_t size) : f_(f), size_(size) {}
  ~FileLineSource() override;
  bool next(std::string& line) override;
  uint32_t bytes_read() const { return read_; }
  uint32_t size() const { return size_; }

 private:
  FILE* f_;
  uint32_t size_, read_ = 0;
};

class Library {
 public:
  explicit Library(std::string root) : root_(std::move(root)) {}

  // Creates the directories. False if the root is not writable.
  bool begin();
  const std::string& root() const { return root_; }

  // Writes a new file (or replaces one) as its bytes arrive, extracting metadata on the way.
  // Nothing is visible under the name until finish() succeeds.
  class Writer {
   public:
    ~Writer();
    bool write(const uint8_t* data, size_t len);
    bool finish(std::string& err);  // closes, publishes, stores metadata
    void abort();                   // deletes everything written
    // Why write() failed: where in the file, and the C library's reason.
    const std::string& error() const { return error_; }

   private:
    friend class Library;
    Writer(Library& lib, std::string name, FILE* f);
    void feed_line();

    Library& lib_;
    std::string name_;
    FILE* f_;
    uint32_t size_ = 0;
    bool failed_ = false, done_ = false;
    std::string error_;
    std::string line_;
    std::vector<std::string> thumbs_opened_;
    MetadataParser parser_;
  };
  std::unique_ptr<Writer> create(const std::string& name, std::string& err);

  // The file names in the library, sorted.
  std::vector<std::string> names() const;
  // Reads an entry; a file with no metadata yet (copied onto the card by hand) is scanned.
  bool get(const std::string& name, LibraryEntry& e);
  bool remove(const std::string& name);
  // Renames a file with its metadata and thumbnails. Refuses to replace an existing file.
  bool rename(const std::string& from, const std::string& to, std::string& err);
  // Records where a hand-off put the file on the printer's card.
  bool set_printer_file(const std::string& name, const std::string& printer_name,
                        uint32_t printer_size);
  std::unique_ptr<FileLineSource> open_lines(const std::string& name) const;
  // The printer names recorded for every file except `except`, upper-case.
  std::vector<std::string> printer_names(const std::string& except) const;

  // True for a name the library accepts: no directories, no leading dot, no control
  // characters, at most 120 bytes.
  static bool valid_name(const std::string& name);
  std::string path(const std::string& name) const { return root_ + "/" + name; }

 private:
  std::string meta_path(const std::string& name) const { return root_ + "/.meta/" + name + ".meta"; }
  bool save(const LibraryEntry& e);
  bool load(const std::string& name, LibraryEntry& e) const;
  void remove_thumbs(const Metadata& m);
  FILE* open_thumb(const std::string& name, int w, int h, std::string& rel);

  std::string root_;
};

// Picks an 8.3 name for the printer's card, "STEM~N.GCO", that is not in `taken`
// (upper-case names). Marlin without LFN_WRITE only accepts 8.3 names in M28.
std::string short_name(const std::string& long_name, const std::vector<std::string>& taken);

}  // namespace halyard
