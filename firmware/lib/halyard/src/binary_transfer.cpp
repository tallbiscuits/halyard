// SPDX-License-Identifier: GPL-3.0-or-later
#include "binary_transfer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gcode_line.h"

namespace halyard {

namespace {

enum : uint8_t { kControl = 0, kFileTransfer = 1 };
enum : uint8_t { kSync = 1, kClose = 2 };                                // control
enum : uint8_t { kQuery = 0, kOpen = 1, kFileClose = 2, kWrite = 3 };  // file (4 = ABORT, unused)

constexpr uint32_t kResendQuietMs = 250;  // one resend per burst of "rs" for the same packet
constexpr int kSyncTries = 3;
constexpr size_t kMaxQueued = 4;           // blocks of payload held before wants_data() says no

// "ok12" -> 12; false for "ok", "ok N3 P15 B3", "ok T:..." (ASCII answers).
bool numbered(const std::string& line, const char* prefix, long& n) {
  size_t k = strlen(prefix);
  if (line.compare(0, k, prefix) != 0 || line.size() == k || !isdigit((unsigned char)line[k]))
    return false;
  n = strtol(line.c_str() + k, nullptr, 10);
  return true;
}

void le16(std::string& s, uint16_t v) {
  s += (char)(v & 0xFF);
  s += (char)(v >> 8);
}

}  // namespace

uint16_t fletcher16(const uint8_t* data, size_t len, uint16_t cs) {
  // As binary_stream.h: both sums mod 255, the high byte folds in the new low one.
  for (size_t i = 0; i < len; i++) {
    uint16_t lo = (uint16_t)(((cs & 0xFF) + data[i]) % 255);
    cs = (uint16_t)(((((cs >> 8) + lo) % 255) << 8) | lo);
  }
  return cs;
}

std::string binary_packet(uint8_t sync, uint8_t protocol, uint8_t type, const std::string& payload) {
  std::string body;
  body += (char)sync;
  body += (char)(((protocol & 0xF) << 4) | (type & 0xF));
  le16(body, (uint16_t)payload.size());
  le16(body, fletcher16((const uint8_t*)body.data(), body.size()));
  if (!payload.empty()) {
    body += payload;
    le16(body, fletcher16((const uint8_t*)body.data(), body.size()));
  }
  std::string p;
  le16(p, 0xB5AD);  // start token, not part of any checksum
  return p + body;
}

BinaryTransfer::BinaryTransfer(Write write, Config cfg) : write_(std::move(write)), cfg_(cfg) {}

// ---- sending ----------------------------------------------------------------------------

void BinaryTransfer::transmit(Pkt& p, uint32_t now) {
  p.sent_at = now;
  write_(p.bytes);
}

void BinaryTransfer::send(uint8_t protocol, uint8_t type, const std::string& payload, uint32_t now) {
  uint8_t sync = next_sync_++;
  Pkt p{sync, binary_packet(sync, protocol, type, payload), now};
  stats_.packets++;
  unacked_.push_back(p);
  transmit(unacked_.back(), now);
}

void BinaryTransfer::send_control(uint8_t protocol, uint8_t type, const std::string& payload,
                                  bool pft, uint32_t now) {
  control_out_ = true;
  control_acked_ = false;
  want_pft_ = pft;
  pft_.clear();
  control_sent_ = now;
  send(protocol, type, payload, now);
}

void BinaryTransfer::resend_all(uint32_t now) {
  last_resend_ = now;
  if (!unacked_.empty()) resent_from_ = unacked_.front().sync;
  window_ = 1;
  clean_acks_ = 0;
  for (auto& p : unacked_) {
    stats_.resent++;
    transmit(p, now);
  }
}

// Marlin acknowledged `sync`, so everything sent before it arrived too.
void BinaryTransfer::acked_through(uint8_t sync) {
  size_t k = 0;
  while (k < unacked_.size() && unacked_[k].sync != sync) k++;
  if (k == unacked_.size()) return;  // a duplicate ok, or one for a packet already counted
  unacked_.erase(unacked_.begin(), unacked_.begin() + k + 1);
  timeouts_ = 0;
  clean_acks_ += k + 1;
  if (clean_acks_ >= cfg_.grow_after && window_ < cfg_.max_window) {
    window_++;
    clean_acks_ = 0;
  }
  stats_.window = window_;
  if (control_out_ && unacked_.empty()) control_acked_ = true;
}

// ---- state machine ----------------------------------------------------------------------

void BinaryTransfer::start(const std::string& filename, uint32_t now) {
  filename_ = filename;
  now_ = now;
  state_ = State::Syncing;
  sync_tries_ = 1;
  sync_sent_ = now;
  write_(binary_packet(0, kControl, kSync));  // any sync number is accepted for SYNC
}

void BinaryTransfer::open_file(uint32_t now) {
  state_ = State::Opening;
  std::string payload;
  payload += '\0';  // not a dummy transfer
  payload += compress_ ? '\1' : '\0';
  payload += filename_;
  payload += '\0';
  send_control(kFileTransfer, kOpen, payload, true, now);
}

void BinaryTransfer::leave(uint32_t now) {
  state_ = State::Leaving;
  send_control(kControl, kClose, "", false, now);
}

void BinaryTransfer::close_file(uint32_t now) {
  state_ = State::Closing;
  send_control(kFileTransfer, kFileClose, "", true, now);
}

void BinaryTransfer::fail(const std::string& why) {
  if (error_.empty()) error_ = why;
  switch (state_) {
    case State::Writing:
      // The file is open: close it. WRITE packets still in flight must be answered first,
      // or the CLOSE would be out of sequence.
      close_pending_ = true;
      return;
    case State::Closing:
      return;  // its answer decides; an error is already recorded
    case State::Query:
    case State::Opening:
      close_pending_ = true;  // settled when the QUERY or OPEN is answered
      return;
    default:
      state_ = State::Failed;  // nothing more to say to the printer, or no way to say it
      return;
  }
}

void BinaryTransfer::abort(const std::string& why) {
  if (finished() || !error_.empty()) return;  // already stopping
  fail(why);
}

void BinaryTransfer::control_done(uint32_t now) {
  control_out_ = false;
  std::string r = pft_;
  switch (state_) {
    case State::Query: {
      // "PFT:version:0.1.0:compression:heatshrink,8,4" or "...:compression:none"
      size_t c = r.find(":compression:");
      std::string algo = c == std::string::npos ? "none" : r.substr(c + 13);
      int w = 0, l = 0;
      compress_ = cfg_.compress && sscanf(algo.c_str(), "heatshrink,%d,%d", &w, &l) == 2 &&
                  w >= 4 && w <= 15 && l >= 3 && l < w;
      if (compress_) {
        window_bits_ = w;
        lookahead_bits_ = l;
      }
      if (close_pending_) return leave(now);
      return open_file(now);
    }
    case State::Opening:
      if (starts_with(r, "PFT:success")) {
        opened_ = true;
        if (close_pending_) {  // failed while opening: close the file we just got
          close_pending_ = false;
          return close_file(now);
        }
        state_ = State::Writing;
        if (compress_) enc_.reset(new HeatshrinkEncoder(window_bits_, lookahead_bits_));
        return;
      }
      if (starts_with(r, "PFT:busy") && !reopen_) {
        // A transfer an earlier connection left open (a file open for writing on the
        // printer): close it, keeping what it wrote, and open ours.
        reopen_ = true;
        closed_stale_ = true;
        return close_file(now);
      }
      if (error_.empty()) error_ = "printer could not open " + filename_ + " (" + r + ")";
      return leave(now);
    case State::Closing:
      if (!starts_with(r, "PFT:success") && error_.empty())
        error_ = "printer could not close the file (" + r + ")";
      if (reopen_ && !opened_ && error_.empty() && !close_pending_) return open_file(now);
      return leave(now);
    case State::Leaving:
      left_ = true;
      state_ = error_.empty() ? State::Done : State::Failed;
      return;
    default:
      return;
  }
}

void BinaryTransfer::on_line(const std::string& line, uint32_t now) {
  now_ = now;
  long n;
  if (numbered(line, "ss", n)) {
    if (state_ != State::Syncing) return;
    // "ss<sync>,<buffer size>,<version>"
    size_t c = line.find(',');
    block_ = c == std::string::npos ? 0 : strtoul(line.c_str() + c + 1, nullptr, 10);
    if (block_ < 16) return fail("printer reported a " + std::to_string(block_) + "-byte buffer");
    next_sync_ = (uint8_t)n;
    state_ = State::Query;
    send_control(kFileTransfer, kQuery, "", true, now);
    return;
  }
  if (numbered(line, "ok", n)) {
    acked_through((uint8_t)n);
  } else if (numbered(line, "rs", n)) {
    stats_.resend_requests++;
    // Marlin wants `n` next: everything before it arrived.
    acked_through((uint8_t)(n - 1));
    // One corrupt packet can draw several requests for the same sync (a stale packet's
    // header garbled too); answer the first, then ignore repeats for a moment.
    bool repeat = resent_from_ == (uint8_t)n && now - last_resend_ < kResendQuietMs;
    if (!unacked_.empty() && !repeat) resend_all(now);
  } else if (starts_with(line, "fe")) {
    // Marlin reset its stream state; its sync is back at 0 and it wants a SYNC.
    unacked_.clear();
    if (error_.empty()) error_ = "printer reported a fatal stream error";
    state_ = State::Failed;
    return;
  } else if (starts_with(line, "PFT:") || starts_with(line, "PTF:")) {  // sic, in 2.1.2.8
    if (state_ == State::Writing && starts_with(line, "PFT:ioerror"))
      return fail("printer's card: write failed");
    if (control_out_ && want_pft_ && pft_.empty()) pft_ = line;
  } else {
    return;  // temperature reports, echo: messages
  }
  if (control_out_ && control_acked_ && (!want_pft_ || !pft_.empty())) control_done(now);
}

void BinaryTransfer::pump(uint32_t now) {
  if (close_pending_) {
    if (!unacked_.empty()) return;
    close_pending_ = false;
    return close_file(now);
  }
  while (unacked_.size() < window_ && block_ &&
         (out_.size() >= block_ || (input_done_ && !out_.empty()))) {
    std::string chunk = out_.substr(0, block_);
    out_.erase(0, chunk.size());
    stats_.wire_payload += chunk.size();
    send(kFileTransfer, kWrite, chunk, now);
  }
  if (input_done_ && out_.empty() && unacked_.empty()) close_file(now);
}

void BinaryTransfer::poll(uint32_t now) {
  now_ = now;
  if (finished()) return;
  if (state_ == State::Syncing) {
    if (now - sync_sent_ < cfg_.ack_timeout_ms) return;
    if (sync_tries_ >= kSyncTries) {
      error_ = "no answer to SYNC: the printer is not in binary mode";
      state_ = State::Failed;
      return;
    }
    sync_tries_++;
    sync_sent_ = now;
    write_(binary_packet(0, kControl, kSync));
    return;
  }
  if (!unacked_.empty() && now - unacked_.front().sent_at >= cfg_.ack_timeout_ms) {
    stats_.timeouts++;
    if (++timeouts_ > cfg_.max_timeouts) {
      if (error_.empty()) error_ = "printer stopped acknowledging packets";
      state_ = State::Failed;  // left_ stays false: the caller must bring the link back
      return;
    }
    resend_all(now);
  }
  if (control_out_ && control_acked_ && want_pft_ && pft_.empty() &&
      now - control_sent_ >= 2 * cfg_.ack_timeout_ms) {
    control_out_ = false;
    if (error_.empty()) error_ = "no answer from the printer's file transfer";
    if (state_ == State::Leaving) {
      state_ = State::Failed;
      return;
    }
    return leave(now);  // whatever it was, try to get Marlin back to ASCII
  }
  if (state_ == State::Writing) pump(now);
}

bool BinaryTransfer::wants_data() const {
  return state_ == State::Writing && !close_pending_ && !input_done_ &&
         out_.size() < kMaxQueued * (block_ ? block_ : 96);
}

void BinaryTransfer::feed(const std::string& bytes) {
  stats_.raw_bytes += bytes.size();
  if (enc_) enc_->write(bytes, out_);
  else out_ += bytes;
}

void BinaryTransfer::end_of_data() {
  if (input_done_) return;
  if (enc_) enc_->finish(out_);
  input_done_ = true;
}

}  // namespace halyard
