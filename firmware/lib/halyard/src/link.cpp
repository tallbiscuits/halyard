// SPDX-License-Identifier: GPL-3.0-or-later
#include "link.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "binary_transfer.h"
#include "gcode_line.h"

namespace halyard {

const char* outcome_name(Outcome o) {
  switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::OkInferred: return "ok (inferred)";
    case Outcome::Lost: return "lost";
    case Outcome::Error: return "error";
    case Outcome::Disconnected: return "disconnected";
  }
  return "?";
}

namespace {

// Value of " <key><digits>" in an ok line, e.g. key 'B' in "ok N5 P15 B3". Requires a digit
// straight after the key, so M105's "B:23.1" does not match.
bool ok_field(const std::string& line, char key, long& value) {
  for (size_t i = 1; i + 1 < line.size(); i++) {
    if (line[i] == key && line[i - 1] == ' ' && isdigit((unsigned char)line[i + 1])) {
      value = strtol(line.c_str() + i + 1, nullptr, 10);
      return true;
    }
  }
  return false;
}

bool printable(const std::string& s) {
  for (unsigned char c : s)
    if ((c < 0x20 && c != '\t') || c > 0x7e) return false;
  return true;
}

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// M303 (PID autotune) runs for minutes without reading the serial port, then empties Marlin's
// RX buffer (queue.flush_rx()). Nothing may be sent behind it, and it goes alone.
bool exclusive(const std::string& cmd) {
  return cmd.size() >= 4 && toupper((unsigned char)cmd[0]) == 'M' && cmd.compare(1, 3, "303") == 0 &&
         (cmd.size() == 4 || cmd[4] == ' ');
}

// Strips a ';' comment and surrounding whitespace. Marlin stops reading a line at ';',
// which would also hide the checksum after it.
std::string clean_command(const std::string& cmd) {
  std::string out;
  sanitize_for_upload(cmd, out);  // only its trimming is wanted here; the verdict is ignored
  return out;
}

size_t digits(long n) { return std::to_string(n).size(); }

}  // namespace

Link::Link(Transport& t) : Link(t, Config()) {}
Link::Link(Transport& t, Config cfg) : t_(t), cfg_(std::move(cfg)) {}

void Link::note(const std::string& s) {
  if (on_note) on_note(s);
}

void Link::set_state(LinkState s) {
  if (s == state_) return;
  state_ = s;
  if (on_state) on_state(s);
}

size_t Link::inflight_bytes() const {
  size_t n = 0;
  for (auto& e : inflight_) n += e.bytes;
  return n;
}

// ---- sending ----------------------------------------------------------------------------

bool Link::send(const std::string& raw, ReplyFn done) {
  if (state_ != LinkState::Ready) return false;
  std::string cmd = clean_command(raw);
  if (cmd.empty() || cmd.find('\\') != std::string::npos || !printable(cmd)) return false;
  // Room for "N<n> " and "*<cs>" within what Marlin keeps of a line.
  long worst_n = next_n_ + (long)pending_.size() + 1000;
  if (cmd.size() + digits(worst_n) + 2 + 4 > kMaxWireLine) return false;
  Entry e;
  e.cmd = std::move(cmd);
  e.done = std::move(done);
  pending_.push_back(std::move(e));
  return true;
}

bool Link::send_now(const std::string& raw, ReplyFn done) {
  if (state_ != LinkState::Ready) return false;
  std::string cmd = clean_command(raw);
  if (cmd.empty() || !printable(cmd)) return false;
  if (recovering_) {
    // Marlin is waiting for a resent line and would reject a new number. The emergency
    // parser still acts on the raw bytes, so send it bare and expect one extra ok.
    tx_ += cmd + "\n";
    extra_oks_++;
    if (done) {
      Reply r;
      r.outcome = Outcome::Lost;
      done(r);
    }
    return true;
  }
  Entry e;
  e.cmd = std::move(cmd);
  e.done = std::move(done);
  transmit(e, now_);
  return true;
}

void Link::transmit(Entry& e, uint32_t now) {
  if (e.n < 0) e.n = next_n_++;
  std::string wire = number_line(e.n, e.cmd) + "\n";
  tx_ += wire;
  e.bytes = wire.size();
  e.sent_at = now;
  if (!e.sent_once) {
    e.sent_once = true;
    e.first_sent = now;
  }
  e.retransmitted = false;
  stats_.lines_sent++;
  stats_.bytes_sent += wire.size();
  inflight_.push_back(std::move(e));
}

void Link::pump(uint32_t now) {
  if (state_ != LinkState::Ready || recovering_) return;
  while (!pending_.empty() && inflight_.size() < window_) {
    Entry& next = pending_.front();
    if (!inflight_.empty() && (exclusive(next.cmd) || exclusive(inflight_.back().cmd))) break;
    long n = next.n >= 0 ? next.n : next_n_;
    size_t bytes = number_line(n, next.cmd).size() + 1;
    if (!inflight_.empty() && inflight_bytes() + bytes > cfg_.byte_budget) break;
    Entry e = std::move(pending_.front());
    pending_.pop_front();
    transmit(e, now);
  }
}

// ---- completion -------------------------------------------------------------------------

void Link::fail_all(Outcome o) {
  std::deque<Entry> dead;
  dead.swap(inflight_);
  for (auto& e : pending_) dead.push_back(std::move(e));
  pending_.clear();
  for (auto& e : dead) complete(e, o);
}

// ---- receiving --------------------------------------------------------------------------

void Link::handle_line(const std::string& line, uint32_t now) {
  last_rx_ = now;
  stats_.lines_received++;
  if (on_line) on_line(line);

  if (!printable(line)) {  // noise, a bad ground, or the ESP's ROM boot log
    stats_.garbled++;
    // Marlin echoes a rejected line as it received it, noise included: a numbered line
    // that noise stripped of its N ran as an unnumbered command. Its ok is not ours, and a
    // garbled echo can never be one of our clean commands (see below).
    if (starts_with(line, "echo:Unknown command: \"") && !inflight_.empty() && !caps_.advanced_ok &&
        state_ == LinkState::Ready)
      swallow_ok_ = true;
    return;
  }
  last_clear_rx_ = now;
  hs_heard_ = true;  // only read while connecting; begin_handshake clears it
  if (state_ == LinkState::Connecting && hs_ == Hs::Query && line != "start") {
    handle_handshake_line(line);
    return;
  }
  if (state_ == LinkState::Connecting && hs_ == Hs::Flush) {
    if (starts_with(line, "ss") && line.size() > 2 && isdigit((unsigned char)line[2]) && !escaped_) {
      escaped_ = true;
      rescued_binary_ = true;
      note("printer was still in binary mode; closing it");
      // First the file transfer's CLOSE, while the file is still open: in binary mode
      // Marlin's OPEN also set flag.saving, so once back in ASCII the M29 below would close
      // the file behind the transfer's back, and with compressed bytes still pending its
      // CLOSE could never succeed again. With no transfer open this CLOSE is answered
      // "PFT:invalid" and touches nothing. Never the ABORT: it deletes card.filename, which
      // is whatever file the card last touched. Then the control CLOSE: back to ASCII.
      uint8_t n = (uint8_t)strtol(line.c_str() + 2, nullptr, 10);
      tx_ += binary_packet(n, 1, 2) + binary_packet((uint8_t)(n + 1), 0, 2) + "\nM29\n";
      hs_since_ = now;  // flush again: the CLOSE's ok, and whatever else it says
    }
    return;
  }

  if (line == "start") {
    note("printer restarted; reconnecting");
    begin_handshake(now);
    return;
  }
  if (line == "ok" || starts_with(line, "ok ")) {
    handle_ok(line, now);
    return;
  }
  if (starts_with(line, "Resend:") || starts_with(line, "rs ")) {
    long r = strtol(line.c_str() + (line[0] == 'R' ? 7 : 3), nullptr, 10);
    handle_resend(r, now);
    return;
  }
  swallow_ok_ = false;

  if (starts_with(line, "Error:")) {
    if (contains(line, "Last Line")) return;  // line-number or checksum error; Resend follows
    if (contains(line, "halted") || contains(line, "kill()") || contains(line, "stopped")) {
      note("printer halted: " + line);
      set_state(LinkState::Halted);
      fail_all(Outcome::Disconnected);
      return;
    }
    if (!inflight_.empty()) {
      inflight_.front().reply.lines.push_back(line);
      inflight_.front().reply.outcome = Outcome::Error;
    }
    return;
  }
  static const char kUnknown[] = "echo:Unknown command: \"";
  if (starts_with(line, kUnknown) && !inflight_.empty() && !caps_.advanced_ok) {
    // When noise turns a byte into a newline, Marlin rejects the first half and empties its
    // RX buffer, but the rest of the line may still be on the wire and then runs as an
    // unnumbered command. Its ok is not ours. (With ADVANCED_OK the missing N shows that.)
    // A command of ours that Marlin doesn't know (M900 without LIN_ADVANCE) is answered
    // with its own name, and its ok is ours.
    std::string what = line.substr(sizeof kUnknown - 1);
    if (!what.empty() && what.back() == '"') what.pop_back();
    if (what != inflight_.front().cmd) {
      swallow_ok_ = true;
      return;
    }
  }
  if (contains(line, "busy:")) {
    last_progress_ = now;
    return;
  }
  std::string t = line;
  while (!t.empty() && t[0] == ' ') t.erase(0, 1);
  if (starts_with(t, "T:")) {  // temperature auto-report; " W:" means M109/M190 is waiting
    // M303 reports every 2 s, and without BUSY_WHILE_HEATING that is its only sign of life.
    if (contains(t, " W:") || (!inflight_.empty() && exclusive(inflight_.front().cmd)))
      last_progress_ = now;
    return;
  }
  if (!inflight_.empty()) inflight_.front().reply.lines.push_back(line);
}

void Link::handle_ok(const std::string& line, uint32_t now) {
  last_progress_ = now;
  long n = -1;
  bool has_n = ok_field(line, 'N', n);
  bool is_m105 = contains(line, "T:");
  // The ok after a Resend is a bare "ok". With ADVANCED_OK every other ok carries P and B,
  // so a bare one is recognisable even when the Resend line itself was garbled.
  bool bare = line == "ok";
  if (swallow_ok_ || (bare && caps_.advanced_ok)) {
    swallow_ok_ = false;
    if (bare || !caps_.advanced_ok) return;
  }
  if (!has_n && !is_m105 && caps_.advanced_ok) {
    // Every line Halyard numbers comes back as "ok N<n>", so this ok is for an unnumbered
    // line: a bare send_now() from a recovery, or the tail of a line split by noise.
    if (extra_oks_ > 0) extra_oks_--;
    return;
  }
  if (inflight_.empty()) {
    stats_.stray_oks++;
    return;
  }
  if (has_n) {
    size_t i = 0;
    while (i < inflight_.size() && inflight_[i].n != n) i++;
    if (i == inflight_.size()) {
      stats_.stray_oks++;
      return;
    }
    // Lines before it were accepted too; their oks were lost on the way back.
    for (size_t k = 0; k < i && !inflight_.empty(); k++) {
      Entry e = std::move(inflight_.front());
      inflight_.pop_front();
      complete(e, Outcome::OkInferred);
    }
    if (inflight_.empty()) return;  // a callback reconnected
  }
  Entry e = std::move(inflight_.front());
  inflight_.pop_front();
  e.reply.ok_line = line;
  // With nothing else of ours queued, B is the whole free command buffer, as at the
  // handshake. That reported B0 if the printer's buffer was full (connecting during a print):
  // the window grows once it drains.
  long b = 0;
  if (caps_.advanced_ok && inflight_.empty() && ok_field(line, 'B', b) && b > (long)window_) {
    window_ = (size_t)b;
    caps_.buffer_free = (int)b;
  }
  complete(e, Outcome::Ok);
}

void Link::complete(Entry& e, Outcome o) {
  if (o == Outcome::Ok && e.reply.outcome == Outcome::Error) o = Outcome::Error;
  e.reply.outcome = o;
  if (o == Outcome::Lost) stats_.lost++;
  if (o == Outcome::OkInferred) stats_.inferred++;
  if (o == Outcome::Ok || o == Outcome::OkInferred || o == Outcome::Error) {
    history_.push_back({e.n, e.cmd});
    if (history_.size() > kHistory) history_.pop_front();
  }
  if (e.done) e.done(e.reply);
}

void Link::handle_resend(long r, uint32_t now) {
  stats_.resend_requests++;
  swallow_ok_ = true;
  if (recovering_ && r == recover_from_) return;  // a stale line rejected in turn

  // Everything numbered r or later is rejected or discarded; send it again once the
  // lines before r have run and the stale ones have drained.
  size_t moved = 0;
  while (!inflight_.empty() && inflight_.back().n >= r) {
    pending_.push_front(std::move(inflight_.back()));
    inflight_.pop_back();
    pending_.front().reply = Reply();
    moved++;
  }
  // Lines already counted as answered can still be the ones Marlin wants, when the
  // Resend that should have stopped them was garbled and its ok was taken for theirs.
  // Marlin's word wins: it never ran them, so they go out again (their callers already
  // heard back, so without callbacks).
  long first = !pending_.empty() && pending_.front().n >= 0 ? pending_.front().n : next_n_;
  for (long k = first - 1; k >= r; k--) {
    auto it = history_.rbegin();
    while (it != history_.rend() && it->first != k) ++it;
    if (it == history_.rend()) break;
    Entry e;
    e.n = k;
    e.cmd = it->second;
    pending_.push_front(std::move(e));
    moved++;
  }
  // r == next_n_: every line sent was accepted, so nothing needs resending. Anything else
  // must be the first line put back, or the numbering has come apart.
  bool satisfiable = r == next_n_ || (!pending_.empty() && pending_.front().n == r);
  if (!satisfiable && (r <= 1 || r > next_n_)) {
    // Line 1: the printer restarted without a readable "start". Beyond what was sent: the
    // numbering is nonsense. Either way the printer is not mid-print with us; start over.
    note("printer asked for line " + std::to_string(r) + ", which is no longer held; reconnecting");
    begin_handshake(now);
    return;
  }
  if (!satisfiable) {
    // Marlin wants a line that was given up as Lost (the wire was cut, say). It never ran
    // that line or any after it, so their Lost stands; renumber what is still held from r
    // instead of a handshake, which could try other baud rates while the printer prints.
    long n = r;
    for (auto& e : pending_)
      if (e.n >= 0) e.n = n++;
    next_n_ = n;
    note("printer asked for line " + std::to_string(r) + ", given up earlier; renumbering from it");
  }
  recovering_ = true;
  recover_from_ = r;
  stats_.recoveries++;
  stats_.lines_resent += moved;
  note("resend from line " + std::to_string(r) + " (" + std::to_string(moved) + " lines)");
}

void Link::check_stall(uint32_t now) {
  if (inflight_.empty()) return;
  Entry& f = inflight_.front();
  uint32_t since = f.sent_at > last_progress_ ? f.sent_at : last_progress_;
  if (now - since < cfg_.stall_ms) return;
  if (!f.retransmitted) {
    // Safe either way: if the printer never got it, it runs now; if it did, Marlin drops a
    // repeat of its last line number silently.
    tx_ += number_line(f.n, f.cmd) + "\n";
    f.retransmitted = true;
    f.sent_at = now;
    stats_.retransmits++;
    note("no reply to line " + std::to_string(f.n) + " (" + f.cmd + "); sending it again");
    return;
  }
  note("giving up on line " + std::to_string(f.n) + " (" + f.cmd + ")");
  Entry e = std::move(inflight_.front());
  inflight_.pop_front();
  bool silent = (int32_t)(last_clear_rx_ - e.first_sent) < 0;
  complete(e, Outcome::Lost);
  if (on_gave_up) on_gave_up(e.cmd, silent);
}

// ---- handshake --------------------------------------------------------------------------

void Link::reconnect() { begin_handshake(now_); }

bool Link::enter_raw(std::function<void(const std::string&)> on_raw) {
  if (state_ != LinkState::Ready || !idle() || raw_) return false;
  raw_ = true;
  on_raw_ = std::move(on_raw);
  return true;
}

void Link::exit_raw() {
  raw_ = false;
  on_raw_ = nullptr;
  last_progress_ = now_;
}

void Link::begin_handshake(uint32_t now) {
  set_state(LinkState::Connecting);
  fail_all(Outcome::Disconnected);
  recovering_ = false;
  swallow_ok_ = false;
  extra_oks_ = 0;
  history_.clear();
  rx_.clear();
  tx_.clear();
  raw_ = false;
  on_raw_ = nullptr;
  escaped_ = false;
  caps_ = Caps();
  caps_.baud = cfg_.bauds[baud_idx_];
  t_.set_baud(caps_.baud);
  // The newline ends whatever junk the printer has half-read (the ESP32-S3 ROM prints its
  // boot log on the UART pins). M29 ends an M28 upload that a previous connection left
  // open; otherwise the handshake lines would be written into that file instead of run.
  // Outside an upload M29 only clears the saving flag. Replies are discarded while flushing.
  // Then a binary SYNC packet: if a transfer was cut off (Halyard reset during M28 B1),
  // Marlin is still in binary mode and hears nothing else. It answers "ss<n>", and
  // handle_line() sends the CLOSE that returns it to ASCII. In ASCII mode those 8 bytes
  // (no newline, ';' or '\\' among them) make one junk line, answered as an unknown command
  // like the ROM's boot noise. It must come after M29: in saving mode the junk line is an
  // error, and Marlin empties its RX buffer on an error, which would take M29 with it.
  tx_ = "\nM29\n" + binary_packet(0, 0, 1) + "\n";
  hs_ = Hs::Flush;
  hs_since_ = now;
  hs_heard_ = false;
}

// M110 N0 and M115 go out together. Their replies are recognised by content, not by counting
// oks: the M115 answer (FIRMWARE_NAME, the Cap: lines, then its ok) is unmistakable, and
// Marlin answers in order, so once it has come everything sent before it has been answered
// too. Counting failed on a printing printer: our lines waited seconds behind the job's, and
// the late oks of the flush were taken for M110's and M115's (handle_handshake_line).
void Link::handshake_poll(uint32_t now) {
  if (hs_ == Hs::Flush) {
    if (now - hs_since_ < cfg_.flush_ms) return;
    hs_ = Hs::Query;
    hs_since_ = now;
    hs_firmware_ = false;
    std::string wire = number_line(0, "M110 N0") + "\nM115\n";
    tx_ += wire;
    stats_.lines_sent += 2;
    stats_.bytes_sent += wire.size();
    return;
  }
  uint32_t last = hs_since_;
  uint32_t wait = caps_.baud == cfg_.known_baud ? cfg_.handshake_known_ms : cfg_.handshake_timeout_ms;
  if (hs_heard_) {
    if ((int32_t)(last_clear_rx_ - last) > 0) last = last_clear_rx_;
    wait = cfg_.handshake_busy_ms;
  }
  if (now - last < wait) return;
  if (!hs_heard_ && ++hs_attempts_ >= 2) {
    hs_attempts_ = 0;
    baud_idx_ = (baud_idx_ + 1) % cfg_.bauds.size();
  }
  begin_handshake(now);
}

// "FIRMWARE_NAME:Marlin 2.1.2.8 (Sep 21 2026) SOURCE_CODE_URL:github.com/... MACHINE_TYPE:3D
// Printer EXTRUDER_COUNT:1": values may hold spaces, so a value runs until the next " KEY:",
// a word of capitals, digits and underscores.
static std::map<std::string, std::string> m115_fields(const std::string& line) {
  std::map<std::string, std::string> out;
  std::vector<std::pair<size_t, size_t>> keys;  // where each key starts, and its colon
  for (size_t i = 0; i < line.size(); i++) {
    if (i > 0 && line[i - 1] != ' ') continue;
    size_t j = i;
    while (j < line.size() && (isupper((unsigned char)line[j]) || isdigit((unsigned char)line[j]) || line[j] == '_')) j++;
    if (j > i && j < line.size() && line[j] == ':' && isupper((unsigned char)line[i])) keys.push_back({i, j});
  }
  for (size_t k = 0; k < keys.size(); k++) {
    size_t from = keys[k].second + 1;
    size_t to = k + 1 < keys.size() ? keys[k + 1].first : line.size();
    std::string v = line.substr(from, to - from);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    out[line.substr(keys[k].first, keys[k].second - keys[k].first)] = v;
  }
  return out;
}

void Link::handle_handshake_line(const std::string& line) {
  if (starts_with(line, "FIRMWARE_NAME:")) {
    size_t end = line.find(" SOURCE_CODE_URL");
    caps_.firmware = line.substr(14, end == std::string::npos ? end : end - 14);
    caps_.cap.clear();
    caps_.info = m115_fields(line);
    hs_firmware_ = true;
    return;
  }
  if (!hs_firmware_) return;  // the flush's replies, our M110's, or an earlier M115's tail
  if (starts_with(line, "Cap:")) {
    size_t colon = line.rfind(':');
    if (colon > 4) caps_.cap[line.substr(4, colon - 4)] = line.substr(colon + 1) == "1";
    return;
  }
  if (line != "ok" && !starts_with(line, "ok ")) return;
  // M115's ok. With ADVANCED_OK it reports free buffer slots: keep that many lines in
  // flight. Without it there is no way to know BUFSIZE: one line at a time.
  long b = 0;
  caps_.advanced_ok = ok_field(line, 'P', b) && ok_field(line, 'B', b);
  caps_.buffer_free = caps_.advanced_ok ? (int)b : 0;
  window_ = caps_.advanced_ok && caps_.buffer_free > 0 ? caps_.buffer_free : 1;
  next_n_ = 1;
  cfg_.known_baud = caps_.baud;
  hs_ = Hs::Done;
  hs_attempts_ = 0;
  stats_.connects++;
  last_progress_ = now_;
  set_state(LinkState::Ready);
}

// ---- main loop --------------------------------------------------------------------------

void Link::poll(uint32_t now) {
  now_ = now;
  if (!started_) {
    started_ = true;
    for (size_t i = 0; i < cfg_.bauds.size(); i++)  // start where the printer last answered
      if (cfg_.bauds[i] == cfg_.known_baud) baud_idx_ = i;
    begin_handshake(now);
  }

  uint8_t buf[256];
  size_t n;
  while ((n = t_.read(buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n') {
        std::string line;
        line.swap(rx_);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (raw_) {
          last_rx_ = now;
          stats_.lines_received++;
          if (on_line) on_line(line);
          if (raw_ && on_raw_) on_raw_(line);
        } else {
          handle_line(line, now);
        }
      } else if (rx_.size() < 1024) {
        rx_ += c;
      }
    }
  }

  if (state_ == LinkState::Connecting) handshake_poll(now);
  if (state_ == LinkState::Ready && !raw_) {
    if (recovering_ && inflight_.empty() && now - last_rx_ >= cfg_.recovery_quiet_ms)
      recovering_ = false;
    check_stall(now);
    pump(now);
  }

  if (!tx_.empty()) {
    size_t w = t_.write((const uint8_t*)tx_.data(), tx_.size());
    tx_.erase(0, w);
  }
}

}  // namespace halyard
