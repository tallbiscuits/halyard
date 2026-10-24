// SPDX-License-Identifier: GPL-3.0-or-later
#include "release.h"

#include <cctype>

namespace halyard {

namespace {

bool is_sha256(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!isxdigit((unsigned char)c)) return false;
  return true;
}

}  // namespace

bool parse_release_sums(const std::string& text, ReleaseSums& out) {
  out = ReleaseSums();
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // "<sha256>  <name>", or "<sha256> *<name>" for a file read in binary mode.
    size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 2 > line.size()) continue;
    std::string sum = line.substr(0, sp), name = line.substr(sp + 2);
    if (!is_sha256(sum)) continue;
    for (char& c : sum) c = (char)tolower((unsigned char)c);
    const std::string pre = "halyard-", ext = ".bin";
    if (name == "firmware.bin") {
      out.firmware_sha256 = sum;
    } else if (name.size() > pre.size() + ext.size() && name.compare(0, pre.size(), pre) == 0 &&
               name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      out.version = name.substr(pre.size(), name.size() - pre.size() - ext.size());
    }
  }
  return !out.version.empty() && !out.firmware_sha256.empty();
}

}  // namespace halyard
