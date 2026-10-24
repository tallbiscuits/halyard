// SPDX-License-Identifier: GPL-3.0-or-later
#include "zip_stream.h"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>

namespace halyard {

namespace {

const uint16_t kFlags = 0x0808;  // sizes in a data descriptor, UTF-8 names

void le16(std::string& s, uint32_t v) {
  s += (char)(v & 0xff);
  s += (char)(v >> 8 & 0xff);
}
void le32(std::string& s, uint32_t v) {
  le16(s, v & 0xffff);
  le16(s, v >> 16);
}

// MS-DOS date and time, as zip keeps them (UTC: Halyard has no time zone).
void dos_time(time_t t, uint16_t& date, uint16_t& time) {
  struct tm tm = {};
  if (t <= 0 || !gmtime_r(&t, &tm) || tm.tm_year < 80) {
    date = 1 << 5 | 1;  // 1980-01-01
    time = 0;
    return;
  }
  date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
  time = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool made = false;
  if (!made) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    made = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

ZipStream::~ZipStream() {
  if (file_) fclose(file_);
}

void ZipStream::add_file(const std::string& name, const std::string& path) {
  Entry e;
  e.name = name;
  e.path = path;
  entries_.push_back(std::move(e));
}

void ZipStream::add_text(const std::string& name, const std::string& text, time_t modified) {
  Entry e;
  e.name = name;
  e.text = text;
  e.is_text = true;
  e.modified = modified;
  entries_.push_back(std::move(e));
}

void ZipStream::put_header(uint32_t sig, const Entry& e, bool central) {
  uint16_t date, time;
  dos_time(e.modified, date, time);
  std::string& s = pending_;
  size_t before = s.size();
  le32(s, sig);
  if (central) le16(s, 20);  // made by: MS-DOS, zip 2.0
  le16(s, 20);               // needed to extract
  le16(s, kFlags);
  le16(s, 0);  // stored
  le16(s, time);
  le16(s, date);
  le32(s, central ? e.crc : 0);
  le32(s, central ? e.size : 0);
  le32(s, central ? e.size : 0);
  le16(s, (uint32_t)e.name.size());
  le16(s, 0);  // extra field
  if (central) {
    le16(s, 0);  // comment
    le16(s, 0);  // disk
    le16(s, 0);  // internal attributes
    le32(s, 0);  // external attributes
    le32(s, e.offset);
  }
  s += e.name;
  offset_ += (uint32_t)(s.size() - before);
}

// The local header for entries_[next_], and its file opened.
bool ZipStream::start_entry() {
  Entry& e = entries_[next_];
  if (!e.is_text) {
    file_ = fopen(e.path.c_str(), "rb");
    if (!file_) return false;
    struct stat st;  // not fstat: the ESP32's FAT fstat leaves the time out
    if (stat(e.path.c_str(), &st) == 0) e.modified = st.st_mtime;
  }
  text_pos_ = 0;
  e.offset = offset_;
  put_header(0x04034b50, e, false);
  in_data_ = true;
  return true;
}

void ZipStream::end_entry() {
  Entry& e = entries_[next_];
  if (file_) fclose(file_);
  file_ = nullptr;
  size_t before = pending_.size();
  le32(pending_, 0x08074b50);
  le32(pending_, e.crc);
  le32(pending_, e.size);
  le32(pending_, e.size);
  offset_ += (uint32_t)(pending_.size() - before);
  in_data_ = false;
  next_++;
}

size_t ZipStream::read(uint8_t* buf, size_t len) {
  size_t done = 0;
  while (done < len && !failed_) {
    if (pending_pos_ < pending_.size()) {
      size_t n = std::min(len - done, pending_.size() - pending_pos_);
      memcpy(buf + done, pending_.data() + pending_pos_, n);
      pending_pos_ += n;
      done += n;
      continue;
    }
    pending_.clear();
    pending_pos_ = 0;
    if (in_data_) {
      Entry& e = entries_[next_];
      size_t n;
      if (e.is_text) {
        n = std::min(len - done, e.text.size() - text_pos_);
        memcpy(buf + done, e.text.data() + text_pos_, n);
        text_pos_ += n;
      } else {
        n = fread(buf + done, 1, len - done, file_);
        if (n == 0 && ferror(file_)) {
          failed_ = true;
          break;
        }
      }
      if (n == 0) {
        end_entry();
        continue;
      }
      e.crc = crc32_update(e.crc, buf + done, n);
      e.size += (uint32_t)n;
      offset_ += (uint32_t)n;
      done += n;
    } else if (next_ < entries_.size()) {
      if (!start_entry()) failed_ = true;
    } else if (!finished_) {
      uint32_t start = offset_;
      for (const Entry& e : entries_) put_header(0x02014b50, e, true);
      uint32_t size = offset_ - start;
      le32(pending_, 0x06054b50);
      le16(pending_, 0);  // this disk
      le16(pending_, 0);  // the central directory's disk
      le16(pending_, (uint32_t)entries_.size());
      le16(pending_, (uint32_t)entries_.size());
      le32(pending_, size);
      le32(pending_, start);
      le16(pending_, 0);  // comment
      finished_ = true;
    } else {
      break;
    }
  }
  return failed_ ? 0 : done;
}

}  // namespace halyard
