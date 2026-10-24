// SPDX-License-Identifier: GPL-3.0-or-later
// A streaming multipart/form-data parser for uploads: the body is fed in chunks as it
// arrives over HTTP, and file data is passed on without ever holding the whole file.
// Portable C++, no Arduino.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace halyard {

class MultipartParser {
 public:
  explicit MultipartParser(const std::string& boundary);

  // The boundary parameter of a "multipart/form-data; boundary=..." Content-Type.
  static bool boundary_from(const std::string& content_type, std::string& boundary);

  // A part starts: its form field name, and the filename if it is a file. Return false to
  // stop the parse.
  std::function<bool(const std::string& name, const std::string& filename)> on_part;
  std::function<bool(const uint8_t* data, size_t len)> on_data;
  std::function<bool()> on_part_end;

  // False once the body is malformed or a callback said stop; error() says why.
  bool feed(const uint8_t* data, size_t len);
  bool done() const { return state_ == State::Done; }
  const std::string& error() const { return error_; }

 private:
  enum class State { Preamble, AfterBoundary, Headers, Body, Done, Failed };
  bool fail(const std::string& why);
  bool step();  // processes buf_ as far as it can; false on failure
  bool parse_headers(const std::string& block);

  std::string dash_boundary_;  // "--" + boundary
  std::string delimiter_;      // "\r\n--" + boundary
  std::string buf_;
  State state_ = State::Preamble;
  std::string error_;
};

}  // namespace halyard
