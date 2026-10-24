// SPDX-License-Identifier: GPL-3.0-or-later
// The link layer: one UART to a Marlin printer, with line numbers, checksums, resend and
// ok-based flow control. Portable C++ (no Arduino); driven by poll() with a caller-supplied
// clock, so it runs the same against the real UART and against the simulated Marlin in tests.
//
// Every line Halyard sends is numbered ("N<n> <cmd>*<cs>"), so every accepted line gets
// exactly one "ok" and a Resend can always be answered. How Marlin behaves, from
// Marlin/src/gcode/queue.cpp (2.1.2.8):
//   - A bad line gets "Error:...", "Resend: <last_N+1>", "ok". Marlin also empties its RX
//     buffer, so later lines already sent vanish without any reply, or arrive later and are
//     rejected in turn (each with its own Error/Resend/ok).
//   - A line numbered last_N or last_N-1 is dropped silently: no error, no ok. That makes
//     retransmitting the oldest unanswered line safe.
//   - "ok" is sent after a command has run, not when it is queued. With ADVANCED_OK it reads
//     "ok N<n> P<planner free> B<buffer free>"; M105 answers "ok T:..." with no N.
//   - Aborting a media print clears the command queue: host lines already queued never
//     get their ok.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace halyard {

// Byte pipe to the printer. Both calls must not block.
struct Transport {
  virtual ~Transport() = default;
  virtual size_t read(uint8_t* buf, size_t max) = 0;
  virtual size_t write(const uint8_t* buf, size_t len) = 0;  // may take fewer bytes
  virtual void set_baud(uint32_t baud) = 0;
};

enum class LinkState { Connecting, Ready, Halted };

enum class Outcome {
  Ok,           // the printer answered "ok"
  OkInferred,   // its ok never arrived, but a later ok proves it was accepted
  Lost,         // no ok ever came (lost on the wire, or dropped by a print abort)
  Error,        // the printer printed "Error:..." while running it
  Disconnected  // the link restarted before it completed (printer reset or halted)
};

const char* outcome_name(Outcome o);

struct Reply {
  Outcome outcome = Outcome::Ok;
  std::vector<std::string> lines;  // lines printed while it ran, "ok" excluded
  std::string ok_line;             // the ok itself; M105's carries the temperatures
};

using ReplyFn = std::function<void(const Reply&)>;

struct Caps {
  std::string firmware;             // FIRMWARE_NAME from M115
  // M115's first line, by key: FIRMWARE_NAME, PROTOCOL_VERSION, MACHINE_TYPE (the
  // machine's name), EXTRUDER_COUNT, UUID, and whatever else the firmware adds.
  std::map<std::string, std::string> info;
  std::map<std::string, bool> cap;  // "Cap:NAME:1" lines
  bool advanced_ok = false;         // not in M115; recognised from the ok format
  int buffer_free = 0;              // B from the handshake ok (BUFSIZE - 1)
  uint32_t baud = 0;
  bool has(const std::string& name) const {
    auto it = cap.find(name);
    return it != cap.end() && it->second;
  }
};

struct LinkStats {
  uint32_t lines_sent = 0, bytes_sent = 0, lines_received = 0;
  uint32_t resend_requests = 0;  // "Resend:" lines, duplicates included
  uint32_t recoveries = 0;       // times the link rewound and resent
  uint32_t lines_resent = 0;
  uint32_t retransmits = 0;      // stall probes: oldest unanswered line sent again
  uint32_t lost = 0, inferred = 0, garbled = 0, stray_oks = 0;
  uint32_t connects = 0;
};

class Link {
 public:
  struct Config {
    std::vector<uint32_t> bauds{250000, 115200};  // tried in order until M110 is answered
    uint32_t flush_ms = 300;          // after the junk-flushing newline, before M110
    // An unanswered handshake is retried, and after two tries the next baud rate is tried.
    // Silence means the wrong baud rate after handshake_timeout_ms, but at the rate the
    // printer last answered at (known_baud, remembered by the glue) only after
    // handshake_known_ms: a busy printer without autoreports can be silent that long. A
    // printer that has said anything legible at this rate is busy, not gone (a print queues
    // its own commands ahead of ours): it gets handshake_busy_ms of silence, and the baud rate
    // is never changed on it.
    uint32_t known_baud = 0;
    uint32_t handshake_timeout_ms = 1500;
    uint32_t handshake_known_ms = 8000;
    uint32_t handshake_busy_ms = 15000;
    uint32_t stall_ms = 5000;         // no ok and no "busy:" for this long: probe, then give up
    uint32_t recovery_quiet_ms = 200; // quiet time before resending after a Resend
    size_t byte_budget = 127;         // unanswered bytes; stock Marlin RX buffers are 128
  };

  explicit Link(Transport& t);
  Link(Transport& t, Config cfg);

  void poll(uint32_t now_ms);
  // The baud rate the printer last answered at (Config::known_baud), from before a restart.
  // Call it before the first poll.
  void set_known_baud(uint32_t baud) { cfg_.known_baud = baud; }

  // Queue a command (no newline, no line number). Returns false if the link is not Ready
  // or the command cannot be sent safely; `done` is then not called.
  bool send(const std::string& cmd, ReplyFn done = nullptr);

  // M108/M112/M410/M524: sent ahead of everything queued and past the window. With
  // EMERGENCY_PARSER Marlin acts on it as the bytes arrive; without it, it still waits in
  // the printer's queue. The caller decides whether that is useful.
  bool send_now(const std::string& cmd, ReplyFn done = nullptr);

  // Drop everything and handshake again.
  void reconnect();

  // Binary mode (after "M28 B1"): lines from the printer go to `on_raw` (and on_line)
  // uninterpreted, and nothing is sent but raw_write(). Needs Ready and idle. exit_raw()
  // goes back to numbered lines; binary mode leaves Marlin's line numbering untouched.
  bool enter_raw(std::function<void(const std::string&)> on_raw);
  void raw_write(const std::string& bytes) { tx_ += bytes; }
  void exit_raw();
  bool raw() const { return raw_; }
  // The last handshake found Marlin still in binary mode and closed it. A transfer may
  // still be open inside Marlin; Printer cleans that up once the printer is known idle.
  bool rescued_binary() const { return rescued_binary_; }
  void clear_rescued_binary() { rescued_binary_ = false; }

  LinkState state() const { return state_; }
  const Caps& caps() const { return caps_; }
  const LinkStats& stats() const { return stats_; }
  size_t queued() const { return pending_.size(); }
  size_t unanswered() const { return inflight_.size(); }
  bool idle() const { return pending_.empty() && inflight_.empty(); }
  size_t window() const { return window_; }

  // Every line received from the printer, before the link interprets it.
  std::function<void(const std::string&)> on_line;
  // State changes (Connecting -> Ready, and so on).
  std::function<void(LinkState)> on_state;
  // Diagnostics worth showing a human: recoveries, stalls, reconnects.
  std::function<void(const std::string&)> on_note;
  // A line got no ok even after it was sent again, and it is dropped as Lost. `silent`:
  // nothing readable has come from the printer since the line was first sent, as when the
  // printer restarted at another baud rate. The link does not reconnect by itself, because
  // a printer that is printing can be silent too (a long G4 without HOST_KEEPALIVE).
  std::function<void(const std::string& cmd, bool silent)> on_gave_up;

 private:
  struct Entry {
    std::string cmd;
    ReplyFn done;
    long n = -1;          // line number, fixed at first transmission
    size_t bytes = 0;     // wire bytes, for the byte budget
    uint32_t sent_at = 0;
    uint32_t first_sent = 0;  // first transmission; a resend does not move it
    bool sent_once = false;
    bool retransmitted = false;
    Reply reply;
  };

  enum class Hs { Flush, Query, Done };

  void set_state(LinkState s);
  void begin_handshake(uint32_t now);
  void handle_handshake_line(const std::string& line);
  void handshake_poll(uint32_t now);
  void handle_line(const std::string& line, uint32_t now);
  void handle_ok(const std::string& line, uint32_t now);
  void handle_resend(long r, uint32_t now);
  void complete(Entry& e, Outcome o);
  void fail_all(Outcome o);
  void transmit(Entry& e, uint32_t now);
  void pump(uint32_t now);
  void check_stall(uint32_t now);
  void note(const std::string& s);
  size_t inflight_bytes() const;

  Transport& t_;
  Config cfg_;
  LinkState state_ = LinkState::Connecting;
  Caps caps_;
  LinkStats stats_;

  std::deque<Entry> pending_;   // not yet on the wire (or rewound by a Resend)
  std::deque<Entry> inflight_;  // on the wire, waiting for ok, in send order
  long next_n_ = 1;
  size_t window_ = 1;

  std::string rx_;
  std::string tx_;
  bool swallow_ok_ = false;     // the ok that follows a Resend belongs to the rejected line
  int extra_oks_ = 0;           // oks due for bare send_now() lines sent during a recovery
  static constexpr size_t kHistory = 32;
  std::deque<std::pair<long, std::string>> history_;  // recently answered lines
  bool recovering_ = false;
  long recover_from_ = 0;
  uint32_t last_rx_ = 0;        // any line received
  uint32_t last_clear_rx_ = 0;  // a printable line received (not noise)
  uint32_t last_progress_ = 0;  // ok or busy received
  uint32_t now_ = 0;

  Hs hs_ = Hs::Flush;
  size_t baud_idx_ = 0;
  int hs_attempts_ = 0;
  bool hs_heard_ = false;       // a legible line at this baud rate, this handshake
  bool hs_firmware_ = false;    // M115's FIRMWARE_NAME line seen
  uint32_t hs_since_ = 0;
  bool started_ = false;
  bool raw_ = false;
  std::function<void(const std::string&)> on_raw_;
  bool escaped_ = false;  // this handshake already took the printer out of binary mode
  bool rescued_binary_ = false;
};

}  // namespace halyard
