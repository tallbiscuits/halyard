// SPDX-License-Identifier: GPL-3.0-or-later
// Marlin's binary file transfer (M28 B1, BINARY_FILE_TRANSFER), the host side. From
// Marlin/src/feature/binary_stream.h (2.1.2.8) and the reference client
// buildroot/share/scripts/MarlinBinaryProtocol.py. Portable C++, no Arduino.
//
// After "M28 B1" Marlin reads every byte on that port as packets until a control CLOSE:
//   AD B5 | sync | protocol<<4 | type | size (LE16) | header fletcher16 (LE16)
//   [ payload | fletcher16 over header and payload (LE16) ]           (token not summed)
// It answers "ok<sync>" per packet, "rs<sync>" to ask for a resend from <sync>, "fe" on a
// fatal error, and "ss<sync>,<buffer>,<version>" to a SYNC, which it accepts whatever the
// sequence. The payload buffer is Marlin's line buffer (MAX_CMD_SIZE, 96 by default).
//
// Unlike the reference client, which waits for each ok, this keeps a few WRITE packets in
// flight (go-back-N). Marlin takes packets from its RX buffer in order, and after asking
// for a resend it drops out-of-sequence packets silently, so resending from <sync> is
// enough. The window starts at one packet and grows while the line stays clean.
//
// It never sends the file-transfer ABORT. Marlin's transfer_abort() deletes card.filename,
// which is whatever file the card last touched: every M20 listing and every file the
// printer's own menu shows overwrites it. A transfer that fails is closed instead (CLOSE
// keeps what was written), and the caller deletes the partial file by name with M30.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "heatshrink_encoder.h"

namespace halyard {

uint16_t fletcher16(const uint8_t* data, size_t len, uint16_t cs = 0);
std::string binary_packet(uint8_t sync, uint8_t protocol, uint8_t type,
                          const std::string& payload = std::string());

class BinaryTransfer {
 public:
  struct Config {
    size_t max_window = 4;       // WRITE packets in flight; ~106 bytes each on the wire
    size_t grow_after = 16;      // clean acks before the window grows by one
    uint32_t ack_timeout_ms = 1000;
    int max_timeouts = 5;        // in a row, then give up
    bool compress = true;        // when Marlin offers heatshrink
  };
  struct Stats {
    uint32_t packets = 0, resent = 0, resend_requests = 0, timeouts = 0;
    uint32_t raw_bytes = 0, wire_payload = 0;  // file bytes in, payload bytes out
    size_t window = 1;
  };
  enum class State { Syncing, Query, Opening, Writing, Closing, Leaving, Done, Failed };

  using Write = std::function<void(const std::string&)>;  // raw bytes to the printer
  BinaryTransfer(Write write, Config cfg);
  explicit BinaryTransfer(Write write) : BinaryTransfer(std::move(write), Config()) {}

  // Starts with a SYNC, so it also works when Marlin was already in binary mode.
  void start(const std::string& filename, uint32_t now_ms);
  // Marlin answered "busy" to the OPEN: a transfer was still open, and it was closed.
  bool closed_stale() const { return closed_stale_; }
  void on_line(const std::string& line, uint32_t now_ms);  // every line while in binary mode
  void poll(uint32_t now_ms);

  // File bytes, uncompressed. Feed while wants_data(); then end_of_data().
  bool wants_data() const;
  void feed(const std::string& bytes);
  void end_of_data();
  // Stops: an open file is closed (the partial file stays), then binary mode is left.
  void abort(const std::string& why);
  // True if the file was opened on the printer and did not complete: delete it by name.
  bool partial_file() const { return opened_ && state_ != State::Done; }

  State state() const { return state_; }
  bool finished() const { return state_ == State::Done || state_ == State::Failed; }
  bool ok() const { return state_ == State::Done; }
  bool left_binary_mode() const { return left_; }  // Marlin confirmed it is back in ASCII
  const std::string& error() const { return error_; }
  const Stats& stats() const { return stats_; }
  bool compressed() const { return compress_; }

 private:
  struct Pkt {
    uint8_t sync;
    std::string bytes;
    uint32_t sent_at;
  };
  void send(uint8_t protocol, uint8_t type, const std::string& payload, uint32_t now);
  void transmit(Pkt& p, uint32_t now);
  void resend_all(uint32_t now);
  void acked_through(uint8_t sync);
  void pump(uint32_t now);
  void fail(const std::string& why);
  void leave(uint32_t now);
  void open_file(uint32_t now);
  void close_file(uint32_t now);
  void control_done(uint32_t now);  // a control packet and its reply both arrived
  void send_control(uint8_t protocol, uint8_t type, const std::string& payload, bool pft,
                    uint32_t now);

  Write write_;
  Config cfg_;
  State state_ = State::Syncing;
  Stats stats_;
  std::string filename_, error_;
  uint8_t next_sync_ = 0;
  std::deque<Pkt> unacked_;
  size_t window_ = 1, clean_acks_ = 0;
  uint32_t now_ = 0, last_resend_ = 0, sync_sent_ = 0;
  uint8_t resent_from_ = 0;
  int timeouts_ = 0, sync_tries_ = 0;
  size_t block_ = 0;             // Marlin's payload buffer, from "ss"
  bool compress_ = false;
  int window_bits_ = 8, lookahead_bits_ = 4;
  std::unique_ptr<HeatshrinkEncoder> enc_;
  std::string out_;              // payload waiting to be packed
  bool input_done_ = false, left_ = false;
  bool close_pending_ = false;   // close once the WRITE packets in flight are answered
  bool reopen_ = false;          // Marlin said busy: close its stale transfer, open again
  bool closed_stale_ = false, opened_ = false;
  // A control packet (not WRITE) waits for its ok and, for FILE_TRANSFER, Marlin's "PFT:".
  bool control_out_ = false, control_acked_ = false, want_pft_ = false;
  uint32_t control_sent_ = 0;
  std::string pft_;
};

}  // namespace halyard
