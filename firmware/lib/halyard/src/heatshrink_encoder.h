// SPDX-License-Identifier: GPL-3.0-or-later
// A streaming encoder for heatshrink's format (LZSS), for Marlin's compressed binary file
// transfer. Written from the bit format that Marlin's decoder reads
// (Marlin/src/libs/heatshrink/heatshrink_decoder.cpp): bits go out MSB first;
//   1 + 8 bits              a literal byte
//   0 + W bits + L bits     a back-reference: (distance - 1), then (length - 1)
// where W and L are the window and lookahead bits Marlin reports in "PFT:version"
// (8 and 4 in 2.1.2.8: up to 256 bytes back, up to 16 bytes long). A back-reference costs
// 1 + W + L bits against 9 per literal, so it is used only when it is shorter.
// Portable C++, no Arduino.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace halyard {

class HeatshrinkEncoder {
 public:
  HeatshrinkEncoder(int window_bits = 8, int lookahead_bits = 4);

  // Appends the compressed form of `data` to `out`. Up to one lookahead of input is held
  // back until more arrives or finish() is called.
  void write(const uint8_t* data, size_t len, std::string& out);
  void write(const std::string& data, std::string& out) {
    write((const uint8_t*)data.data(), data.size(), out);
  }
  // Encodes what is held back and pads the last byte with zero bits (which the decoder
  // cannot mistake for a token: a whole token needs more bits than the padding holds).
  void finish(std::string& out);

 private:
  void encode(bool final, std::string& out);
  void put_bits(uint32_t value, int count, std::string& out);

  const int window_bits_, lookahead_bits_;
  const size_t window_, max_len_, min_len_;
  std::vector<uint8_t> buf_;  // up to one window of history, then input not yet encoded
  size_t pos_ = 0;            // first byte of buf_ not yet encoded
  uint32_t bits_ = 0;
  int nbits_ = 0;
};

}  // namespace halyard
