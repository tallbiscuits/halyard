// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_line.h"

#include <cctype>
#include <cstring>

namespace halyard {

uint8_t checksum(const std::string& s) {
  uint8_t cs = 0;
  for (unsigned char c : s) cs ^= c;
  return cs;
}

std::string number_line(long n, const std::string& cmd) {
  std::string body = "N" + std::to_string(n) + " " + cmd;
  return body + "*" + std::to_string(checksum(body));
}

static bool is_m29(const std::string& s) {
  // Marlin's is_M29(): "M29" anywhere, not followed by a digit (queue.cpp).
  for (size_t p = s.find("M29"); p != std::string::npos; p = s.find("M29", p + 1))
    if (p + 3 >= s.size() || !isdigit((unsigned char)s[p + 3])) return true;
  return false;
}

const char* sanitize_for_upload(const std::string& raw, std::string& out) {
  size_t end = raw.find(';');
  if (end == std::string::npos) end = raw.size();
  size_t begin = 0;
  while (begin < end && isspace((unsigned char)raw[begin])) begin++;
  while (end > begin && isspace((unsigned char)raw[end - 1])) end--;
  out.assign(raw, begin, end - begin);
  for (unsigned char c : out) {
    if (c < 0x20 || c > 0x7e) return "non-printable character";
    if (c == '\\') return "backslash (Marlin drops it, breaking the checksum)";
    if (c == '*') return "'*' (Marlin's M28 cuts the line at the first '*')";
  }
  if (!out.empty() && out[0] == 'N') return "line starts with its own line number";
  if (is_m29(out)) return "contains M29, which would end the upload";
  return nullptr;
}

}  // namespace halyard
