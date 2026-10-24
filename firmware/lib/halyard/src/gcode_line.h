// SPDX-License-Identifier: GPL-3.0-or-later
// G-code line helpers shared by the link and the upload path. Portable C++, no Arduino.
#pragma once
#include <cstdint>
#include <string>

namespace halyard {

// Marlin's default MAX_CMD_SIZE is 96; it keeps at most 95 characters of a line and silently
// drops the rest, including the checksum. Stay one under that, since other builds may differ.
constexpr size_t kMaxWireLine = 94;

// XOR of every byte, as Marlin computes it over "N<n> <cmd>" (queue.cpp).
uint8_t checksum(const std::string& s);

// "N<n> <cmd>*<checksum>" (no newline).
std::string number_line(long n, const std::string& cmd);

// Makes one line of a G-code file safe to write with M28, or explains why it cannot be.
// Strips the ';' comment and surrounding whitespace. On success `out` holds the text Marlin
// will write to its card (it may be empty: skip the line). Returns nullptr on success or a
// reason. Rejected are characters Marlin's line reader alters, which would break the
// checksum ('\\', non-printables) or which M28 misreads ('*', "M29" inside a line).
const char* sanitize_for_upload(const std::string& raw, std::string& out);

// Case-sensitive "starts with".
inline bool starts_with(const std::string& s, const char* prefix) {
  return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

}  // namespace halyard
