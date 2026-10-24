// SPDX-License-Identifier: GPL-3.0-or-later
// A Halyard release's SHA256SUMS (sha256sum's output, written by ./run.sh release): which
// version the release is, from its merged image's name (halyard-<version>.bin), and the
// checksum of its firmware.bin, which an update over WiFi checks before switching to it.
// Portable C++, no Arduino.
#pragma once
#include <string>

namespace halyard {

struct ReleaseSums {
  std::string version;          // "0.6.0"
  std::string firmware_sha256;  // 64 lowercase hex digits
};

// False if the text names no version or no firmware.bin.
bool parse_release_sums(const std::string& text, ReleaseSums& out);

}  // namespace halyard
