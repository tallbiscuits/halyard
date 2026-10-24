// SPDX-License-Identifier: GPL-3.0-or-later
// Mainsail's release zip, as it downloads, into the ustar tar that WebUi::Install takes.
// Each deflated entry becomes NAME.gz: a gzip header, the zip's own deflate bytes, then the
// CRC and size the zip already records. Stored entries (images, fonts) stay as they are.
// Nothing is decompressed, so the ESP32 needs no zlib and little memory to install Mainsail
// straight from GitHub. The result matches what ./run.sh webui sends, file for file.
//
// Only zips that give each entry's sizes up front, which Mainsail's releases do; a zip with
// data descriptors, ZIP64 or encryption is refused. Portable C++.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace halyard {

class ZipToTar {
 public:
  using Out = std::function<bool(const uint8_t* data, size_t len)>;  // false: stop
  explicit ZipToTar(Out out) : out_(std::move(out)) {}

  bool feed(const uint8_t* data, size_t len);  // false: stop; error() says why
  bool finish();  // after the last byte: false if the zip was cut off
  const std::string& error() const { return error_; }
  int files() const { return files_; }

 private:
  enum class State { Header, Name, Data, Done };
  bool fail(const std::string& msg);
  bool emit(const uint8_t* d, size_t n);
  bool start_entry();
  bool end_entry();

  Out out_;
  State state_ = State::Header;
  uint8_t head_[30];
  size_t have_ = 0;
  std::string name_;     // name and extra field, while collecting them
  size_t name_len_ = 0, extra_len_ = 0;
  uint16_t method_ = 0;
  uint32_t crc_ = 0, comp_ = 0, size_ = 0, left_ = 0, written_ = 0;
  bool skip_ = false;    // a directory entry
  int files_ = 0;
  std::string error_;
};

}  // namespace halyard
