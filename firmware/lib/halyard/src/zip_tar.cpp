// SPDX-License-Identifier: GPL-3.0-or-later
#include "zip_tar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace halyard {

namespace {

uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// A ustar header; a name over 100 bytes is split into prefix/name at a '/'.
bool tar_header(const std::string& path, uint32_t size, uint8_t h[512]) {
  std::string name = path, prefix;
  if (name.size() > 100) {
    size_t cut = name.rfind('/', 155);
    if (cut == std::string::npos || name.size() - cut - 1 > 100) return false;
    prefix = name.substr(0, cut);
    name = name.substr(cut + 1);
  }
  memset(h, 0, 512);
  memcpy(h, name.data(), name.size());
  memcpy(h + 100, "0000644", 7);
  memcpy(h + 108, "0000000", 7);
  memcpy(h + 116, "0000000", 7);
  snprintf((char*)h + 124, 12, "%011o", (unsigned)size);
  memcpy(h + 136, "00000000000", 11);
  h[156] = '0';
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  memcpy(h + 345, prefix.data(), prefix.size());
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += h[i];
  snprintf((char*)h + 148, 8, "%06o", sum);
  return true;
}

const uint8_t kZeros[512] = {};

}  // namespace

bool ZipToTar::fail(const std::string& msg) {
  if (error_.empty()) error_ = msg;
  state_ = State::Done;
  return false;
}

bool ZipToTar::emit(const uint8_t* d, size_t n) {
  if (n == 0) return true;
  if (!out_(d, n)) return fail("the install stopped");
  written_ += n;
  return true;
}

// The local header and name are in: write the tar header, and the gzip header if deflated.
bool ZipToTar::start_entry() {
  std::string name = name_.substr(0, name_len_);
  if (name.empty() || name.back() == '/') {  // a directory: tar makes them as needed
    skip_ = true;
    left_ = comp_;
    return true;
  }
  skip_ = false;
  left_ = comp_;
  written_ = 0;
  bool gz = method_ == 8;
  uint8_t h[512];
  if (!tar_header(gz ? name + ".gz" : name, gz ? comp_ + 18 : comp_, h))
    return fail(name + ": name too long");
  if (!emit(h, 512)) return false;
  written_ = 0;  // counts the entry's data, for the padding
  files_++;
  if (gz) {
    static const uint8_t gzip_head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    return emit(gzip_head, sizeof gzip_head);
  }
  return true;
}

bool ZipToTar::end_entry() {
  if (skip_) return true;
  if (method_ == 8) {
    uint8_t tail[8] = {(uint8_t)crc_,  (uint8_t)(crc_ >> 8),  (uint8_t)(crc_ >> 16),  (uint8_t)(crc_ >> 24),
                       (uint8_t)size_, (uint8_t)(size_ >> 8), (uint8_t)(size_ >> 16), (uint8_t)(size_ >> 24)};
    if (!emit(tail, sizeof tail)) return false;
  }
  return emit(kZeros, (512 - written_ % 512) % 512);
}

bool ZipToTar::feed(const uint8_t* d, size_t n) {
  while (n > 0) {
    switch (state_) {
      case State::Done:
        if (!error_.empty()) return false;
        return true;  // the central directory and the end record: not needed

      case State::Header: {
        size_t take = std::min(n, sizeof head_ - have_);
        memcpy(head_ + have_, d, take);
        have_ += take;
        d += take;
        n -= take;
        if (have_ >= 4 && le32(head_) != 0x04034b50) {
          if (le32(head_) == 0x02014b50 || le32(head_) == 0x06054b50) {  // the directory
            state_ = State::Done;
            return emit(kZeros, 512) && emit(kZeros, 512);
          }
          return fail(files_ ? "the zip file is damaged" : "this is not a zip file");
        }
        if (have_ < sizeof head_) break;
        uint16_t flags = le16(head_ + 6);
        method_ = le16(head_ + 8);
        crc_ = le32(head_ + 14);
        comp_ = le32(head_ + 18);
        size_ = le32(head_ + 22);
        name_len_ = le16(head_ + 26);
        extra_len_ = le16(head_ + 28);
        if (flags & 1) return fail("an encrypted zip");
        if (flags & 8) return fail("a zip without sizes in its local headers (data descriptors)");
        if (comp_ == 0xFFFFFFFF || size_ == 0xFFFFFFFF) return fail("a ZIP64 file");
        if (method_ != 0 && method_ != 8) return fail("a zip compression other than deflate");
        name_.clear();
        have_ = 0;
        state_ = State::Name;
        break;
      }

      case State::Name: {
        size_t take = std::min(n, name_len_ + extra_len_ - name_.size());
        name_.append((const char*)d, take);
        d += take;
        n -= take;
        if (name_.size() < name_len_ + extra_len_) break;
        if (!start_entry()) return false;
        state_ = State::Data;
        if (left_ == 0) {
          if (!end_entry()) return false;
          state_ = State::Header;
        }
        break;
      }

      case State::Data: {
        size_t take = std::min<size_t>(n, left_);
        if (!skip_ && !emit(d, take)) return false;
        d += take;
        n -= take;
        left_ -= take;
        if (left_ == 0) {
          if (!end_entry()) return false;
          state_ = State::Header;
        }
        break;
      }
    }
  }
  return error_.empty();
}

bool ZipToTar::finish() {
  if (!error_.empty()) return false;
  if (state_ != State::Done) return fail("the download was cut off");
  return true;
}

}  // namespace halyard
