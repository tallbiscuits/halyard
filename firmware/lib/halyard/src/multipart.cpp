// SPDX-License-Identifier: GPL-3.0-or-later
#include "multipart.h"

#include <cctype>
#include <cstring>

namespace halyard {

namespace {

const size_t kMaxHeaders = 4096;

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// The value of `key` in a header's parameters: `name="file"; filename="a b.gcode"`.
bool param(const std::string& header, const std::string& key, std::string& out) {
  std::string l = lower(header);
  size_t pos = 0;
  while ((pos = l.find(key + "=", pos)) != std::string::npos) {
    // Whole parameter names only: "filename=" must not match inside "xfilename=".
    if (pos > 0 && l[pos - 1] != ' ' && l[pos - 1] != ';' && l[pos - 1] != '\t') {
      pos += key.size();
      continue;
    }
    size_t v = pos + key.size() + 1;
    out.clear();
    if (v < header.size() && header[v] == '"') {
      for (size_t i = v + 1; i < header.size(); i++) {
        if (header[i] == '\\' && i + 1 < header.size()) {
          out += header[++i];
        } else if (header[i] == '"') {
          return true;
        } else {
          out += header[i];
        }
      }
      return false;  // unterminated quote
    }
    size_t e = header.find(';', v);
    out = header.substr(v, e == std::string::npos ? std::string::npos : e - v);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return true;
  }
  return false;
}

}  // namespace

MultipartParser::MultipartParser(const std::string& boundary)
    : dash_boundary_("--" + boundary), delimiter_("\r\n--" + boundary) {}

bool MultipartParser::boundary_from(const std::string& content_type, std::string& boundary) {
  if (lower(content_type).find("multipart/form-data") == std::string::npos) return false;
  return param(content_type, "boundary", boundary) && !boundary.empty() && boundary.size() <= 70;
}

bool MultipartParser::fail(const std::string& why) {
  if (state_ != State::Failed) error_ = why;
  state_ = State::Failed;
  buf_.clear();
  return false;
}

bool MultipartParser::feed(const uint8_t* data, size_t len) {
  if (state_ == State::Failed) return false;
  if (state_ == State::Done) return true;  // the epilogue is ignored
  buf_.append((const char*)data, len);
  return step();
}

bool MultipartParser::parse_headers(const std::string& block) {
  std::string name, filename;
  bool disposition = false;
  size_t pos = 0;
  while (pos < block.size()) {
    size_t e = block.find("\r\n", pos);
    if (e == std::string::npos) e = block.size();
    std::string h = block.substr(pos, e - pos);
    pos = e + 2;
    size_t colon = h.find(':');
    if (colon == std::string::npos) continue;
    if (lower(h.substr(0, colon)) != "content-disposition") continue;
    disposition = true;
    std::string v = h.substr(colon + 1);
    if (!param(v, "name", name)) return fail("part without a name");
    param(v, "filename", filename);
  }
  if (!disposition) return fail("part without Content-Disposition");
  if (on_part && !on_part(name, filename)) return fail("upload refused");
  return true;
}

bool MultipartParser::step() {
  for (;;) {
    switch (state_) {
      case State::Preamble: {
        size_t at = buf_.find(dash_boundary_);
        if (at == std::string::npos) {
          if (buf_.size() > dash_boundary_.size()) buf_.erase(0, buf_.size() - dash_boundary_.size());
          return true;
        }
        buf_.erase(0, at + dash_boundary_.size());
        state_ = State::AfterBoundary;
        break;
      }
      case State::AfterBoundary: {
        // Transport padding may follow a boundary, then CRLF (another part) or "--" (the end).
        size_t i = 0;
        while (i < buf_.size() && (buf_[i] == ' ' || buf_[i] == '\t')) i++;
        if (buf_.size() < i + 2) return true;
        if (buf_.compare(i, 2, "--") == 0) {
          state_ = State::Done;
          buf_.clear();
          return true;
        }
        if (buf_.compare(i, 2, "\r\n") != 0) return fail("malformed boundary");
        buf_.erase(0, i + 2);
        state_ = State::Headers;
        break;
      }
      case State::Headers: {
        size_t end = buf_.find("\r\n\r\n");
        if (buf_.compare(0, 2, "\r\n") == 0) end = 0;  // a part with no headers at all
        if (end == std::string::npos) {
          if (buf_.size() > kMaxHeaders) return fail("part headers too long");
          return true;
        }
        std::string block = buf_.substr(0, end);
        buf_.erase(0, end == 0 ? 2 : end + 4);
        if (!parse_headers(block)) return false;
        state_ = State::Body;
        break;
      }
      case State::Body: {
        size_t at = buf_.find(delimiter_);
        if (at == std::string::npos) {
          // Everything except a tail that could be the start of the delimiter.
          size_t keep = delimiter_.size() - 1;
          if (buf_.size() > keep) {
            size_t n = buf_.size() - keep;
            if (on_data && !on_data((const uint8_t*)buf_.data(), n)) return fail("write failed");
            buf_.erase(0, n);
          }
          return true;
        }
        if (at > 0 && on_data && !on_data((const uint8_t*)buf_.data(), at))
          return fail("write failed");
        buf_.erase(0, at + delimiter_.size());
        if (on_part_end && !on_part_end()) return fail("upload refused");
        state_ = State::AfterBoundary;
        break;
      }
      case State::Done:
        return true;
      case State::Failed:
        return false;
    }
  }
}

}  // namespace halyard
