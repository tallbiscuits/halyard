// SPDX-License-Identifier: GPL-3.0-or-later
// A zip archive made while it downloads (server.files.zip, for Mainsail's download of
// several files). Entries are stored, not compressed, and each one's CRC and size follow its
// data in a data descriptor, so every file is read once and nothing is written to the card.
// Names are UTF-8. No ZIP64: the service refuses more than 4 GB. Portable C++.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace halyard {

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);  // start with 0

class ZipStream {
 public:
  ZipStream() = default;
  ZipStream(const ZipStream&) = delete;
  ZipStream& operator=(const ZipStream&) = delete;
  ~ZipStream();

  void add_file(const std::string& name, const std::string& path);  // read when its turn comes
  void add_text(const std::string& name, const std::string& text, time_t modified);

  // The next bytes of the archive, at most `len`; 0 at the end, or when a file could not be
  // read (failed() then says so, and the archive must not be sent as complete).
  size_t read(uint8_t* buf, size_t len);
  bool failed() const { return failed_; }

 private:
  struct Entry {
    std::string name, path, text;
    bool is_text = false;
    time_t modified = 0;
    uint32_t crc = 0, size = 0, offset = 0;
  };
  bool start_entry();
  void end_entry();
  void put_header(uint32_t sig, const Entry& e, bool central);

  std::vector<Entry> entries_;
  size_t next_ = 0;         // the entry whose data comes next
  bool in_data_ = false;    // between an entry's header and its descriptor
  bool finished_ = false;   // the central directory is queued
  bool failed_ = false;
  FILE* file_ = nullptr;
  size_t text_pos_ = 0;
  std::string pending_;     // header bytes not yet read
  size_t pending_pos_ = 0;
  uint32_t offset_ = 0;     // bytes of the archive so far
};

}  // namespace halyard
