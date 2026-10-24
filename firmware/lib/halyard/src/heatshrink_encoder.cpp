// SPDX-License-Identifier: GPL-3.0-or-later
#include "heatshrink_encoder.h"

namespace halyard {

namespace {
constexpr size_t kCompactAt = 4096;  // drop old history once this much has been encoded
}

HeatshrinkEncoder::HeatshrinkEncoder(int window_bits, int lookahead_bits)
    : window_bits_(window_bits),
      lookahead_bits_(lookahead_bits),
      window_(size_t(1) << window_bits),
      max_len_(size_t(1) << lookahead_bits),
      // Shortest back-reference that beats literals: 1 + W + L bits against 9 per byte.
      min_len_((1 + window_bits + lookahead_bits) / 9 + 1) {}

void HeatshrinkEncoder::put_bits(uint32_t value, int count, std::string& out) {
  for (int i = count - 1; i >= 0; i--) {
    bits_ = (bits_ << 1) | ((value >> i) & 1);
    if (++nbits_ == 8) {
      out += (char)(uint8_t)bits_;
      bits_ = 0;
      nbits_ = 0;
    }
  }
}

void HeatshrinkEncoder::write(const uint8_t* data, size_t len, std::string& out) {
  buf_.insert(buf_.end(), data, data + len);
  encode(false, out);
}

void HeatshrinkEncoder::finish(std::string& out) {
  encode(true, out);
  if (nbits_) put_bits(0, 8 - nbits_, out);
}

void HeatshrinkEncoder::encode(bool final, std::string& out) {
  // Without `final`, keep a full lookahead in hand so every match is as long as it can be.
  while (pos_ < buf_.size() && (final || buf_.size() - pos_ >= max_len_)) {
    size_t avail = buf_.size() - pos_;
    size_t limit = avail < max_len_ ? avail : max_len_;
    size_t best_len = 0, best_dist = 0;
    size_t lo = pos_ > window_ ? pos_ - window_ : 0;
    const uint8_t* cur = &buf_[pos_];
    for (size_t j = pos_; j-- > lo;) {  // nearest first; a tie keeps the nearer one
      if (buf_[j] != cur[0]) continue;
      size_t n = 1;
      // The match may run into the bytes it produces (j + n >= pos_), as the decoder
      // copies one byte at a time.
      while (n < limit && buf_[j + n] == cur[n]) n++;
      if (n > best_len) {
        best_len = n;
        best_dist = pos_ - j;
        if (n == limit) break;
      }
    }
    if (best_len >= min_len_) {
      put_bits(0, 1, out);
      put_bits((uint32_t)(best_dist - 1), window_bits_, out);
      put_bits((uint32_t)(best_len - 1), lookahead_bits_, out);
      pos_ += best_len;
    } else {
      put_bits(1, 1, out);
      put_bits(cur[0], 8, out);
      pos_++;
    }
  }
  if (pos_ >= kCompactAt + window_) {
    size_t drop = pos_ - window_;
    buf_.erase(buf_.begin(), buf_.begin() + drop);
    pos_ -= drop;
  }
}

}  // namespace halyard
