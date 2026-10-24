// SPDX-License-Identifier: GPL-3.0-or-later
// Link-layer tests against the simulated Marlin in fake_marlin.h. Run: pio test -e native
#include <unity.h>

#include <functional>
#include <memory>

#include "fake_marlin.h"
#include "gcode_line.h"
#include "link.h"
#include "printer.h"

using namespace halyard;

struct Rig {
  FakeMarlin m;
  FakeTransport t{m};
  Link link;
  Printer printer;
  uint32_t now = 1;
  std::vector<std::string> events, notes;

  explicit Rig(FakeMarlin::Options o = {}, Link::Config c = {}) : link(t, c), printer(link) {
    m.opt = o;
    printer.on_event = [this](const std::string& e) { events.push_back(e); };
    link.on_note = [this](const std::string& n) { notes.push_back(n); };
  }
  void step() {
    printer.poll(now);
    m.tick(now);
    now++;
  }
  bool run_until(const std::function<bool()>& cond, uint32_t max_ms) {
    for (uint32_t i = 0; i < max_ms; i++) {
      if (cond()) return true;
      step();
    }
    return cond();
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) step();
  }
  bool connect() {
    return run_until([&] { return link.state() == LinkState::Ready; }, 10000);
  }
  // Uploads a test job; returns the result message, with "OK: " or "FAIL: " in front.
  std::string upload(const std::string& name, int seconds, uint32_t max_ms = 120000) {
    bool done = false;
    std::string msg;
    bool started = printer.upload(name, std::unique_ptr<LineSource>(new TestJobSource(seconds)),
                                  [&](bool ok, const std::string& m2) {
                                    done = true;
                                    msg = (ok ? "OK: " : "FAIL: ") + m2;
                                  });
    if (!started) return "FAIL: not started";
    if (!run_until([&] { return done; }, max_ms)) return "FAIL: timed out";
    return msg;
  }
};

static std::string expected_file(int seconds) {
  TestJobSource src(seconds);
  std::string raw, clean, all;
  while (src.next(raw)) {
    sanitize_for_upload(raw, clean);
    if (!clean.empty()) all += clean + "\r\n";
  }
  return all;
}

static void assert_starts(const char* prefix, const std::string& s) {
  TEST_ASSERT_TRUE_MESSAGE(starts_with(s, prefix), s.c_str());
}

// ---- line format ------------------------------------------------------------------------

void test_checksum_and_numbering() {
  // Checked against Marlin's rule: XOR of every byte before '*'.
  TEST_ASSERT_EQUAL_STRING("N0 M110 N0*125", number_line(0, "M110 N0").c_str());
  TEST_ASSERT_EQUAL_UINT8('N' ^ '1' ^ ' ' ^ 'M' ^ '1' ^ '0' ^ '5', checksum("N1 M105"));
}

void test_sanitize() {
  std::string out;
  TEST_ASSERT_NULL(sanitize_for_upload("  G1 X10 ; move  ", out));
  TEST_ASSERT_EQUAL_STRING("G1 X10", out.c_str());
  TEST_ASSERT_NULL(sanitize_for_upload("; only a comment", out));
  TEST_ASSERT_EQUAL_STRING("", out.c_str());
  TEST_ASSERT_NOT_NULL(sanitize_for_upload("M117 a*b", out));
  TEST_ASSERT_NOT_NULL(sanitize_for_upload("M117 a\\b", out));
  TEST_ASSERT_NOT_NULL(sanitize_for_upload("M117 run M29 now", out));
  TEST_ASSERT_NULL(sanitize_for_upload("M290 Z0.1", out));  // M290 is not M29
  TEST_ASSERT_NOT_NULL(sanitize_for_upload("N5 G1 X1", out));
}

// ---- handshake --------------------------------------------------------------------------

void test_handshake_full_features() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  const Caps& c = r.link.caps();
  TEST_ASSERT_TRUE(c.advanced_ok);
  TEST_ASSERT_EQUAL(3, c.buffer_free);
  TEST_ASSERT_EQUAL(3, (int)r.link.window());
  TEST_ASSERT_TRUE(c.has("EMERGENCY_PARSER"));
  TEST_ASSERT_FALSE(c.has("BINARY_FILE_TRANSFER"));
  TEST_ASSERT_EQUAL_UINT32(250000, c.baud);
  assert_starts("Marlin 2.1.2.8", c.firmware);
}

void test_handshake_minimal_firmware() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_FALSE(r.link.caps().advanced_ok);
  TEST_ASSERT_EQUAL(1, (int)r.link.window());
  TEST_ASSERT_FALSE(r.link.caps().has("AUTOREPORT_TEMP"));
  // Temperatures still arrive, by polling M105.
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().temps_valid; }, 5000));
}

void test_handshake_finds_baud() {
  FakeMarlin::Options o;
  o.baud = 115200;
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_EQUAL_UINT32(115200, r.link.caps().baud);
}

void test_handshake_through_boot_junk() {
  Rig r;
  const char junk[] = "\xE0\x80\x18\xF8\x38\x80";  // what the ESP32-S3 ROM log looks like at 250k
  for (const char* p = junk; *p; p++) r.m.rx.push_back((uint8_t)*p);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_EQUAL(0, r.m.errors);
}

void test_reconnects_after_printer_reset() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.m.reboot();
  r.run(50);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_EQUAL_UINT32(2, r.link.stats().connects);
}

static FakeMarlin::Options minimal_options() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = false;
  o.long_filename = o.keepalive = false;
  o.busy_while_heating = o.pid_bed = o.probe = o.leveling = false;
  o.baud = 115200;
  o.rx_size = 128;
  return o;
}

static bool has_event(const Rig& r, const char* text) {
  for (auto& e : r.events)
    if (e.find(text) != std::string::npos) return true;
  return false;
}

void test_reconnects_when_idle_printer_returns_at_other_baud() {
  // Reflashed while Halyard stayed powered: the printer comes back at 115200 and its "start"
  // arrives as noise at 250000, so nothing tells the link it restarted.
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.run(5000);  // idle, polled
  TEST_ASSERT_EQUAL(Job::Idle, r.printer.status().job);
  r.m.opt = minimal_options();
  r.m.reboot();
  TEST_ASSERT_TRUE(r.run_until([&] {
    return r.link.state() == LinkState::Ready && r.link.caps().baud == 115200;
  }, 60000));
  TEST_ASSERT_TRUE(has_event(r, "was idle; reconnecting"));
  TEST_ASSERT_FALSE(r.link.caps().advanced_ok);
  assert_starts("OK:", r.upload("AFTER.GCO", 2));
}

void test_no_reconnect_when_printer_goes_quiet_mid_print() {
  // A printer that is printing may be silent for a long time (a long G4, no keepalive).
  // Reconnecting would try other baud rates: noise into a running print. Only report it.
  Rig r(minimal_options());
  TEST_ASSERT_TRUE(r.connect());
  std::string job;
  for (int i = 0; i < 120; i++) job += "M117 x\r\nG4 S1\r\n";
  r.m.files["LONG.GCO"] = job;
  bool started = false;
  r.printer.start_print("LONG.GCO", [&](bool ok, const std::string&) { started = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return started; }, 5000));
  r.run(3000);
  r.m.unplugged = true;
  r.run(30000);
  TEST_ASSERT_TRUE(has_event(r, "not reconnecting on my own"));
  TEST_ASSERT_FALSE(has_event(r, "reconnecting;") || has_event(r, "; reconnecting"));
  TEST_ASSERT_EQUAL_UINT32(1, r.link.stats().connects);
  TEST_ASSERT_FALSE(r.printer.status().responding);
  TEST_ASSERT_TRUE(r.m.sd_printing);  // untouched
  r.m.unplugged = false;
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().responding; }, 10000));
  TEST_ASSERT_EQUAL(Job::Printing, r.printer.status().job);
}

void test_notices_print_started_on_printer() {
  // Started from the printer's own screen: the idle poll sees it.
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.run(3000);
  TEST_ASSERT_EQUAL(Job::Idle, r.printer.status().job);
  std::string job;
  for (int i = 0; i < 10; i++) job += "M117 a\r\nG4 S1\r\n";  // polls wait behind each G4
  r.m.files["LCD.GCO"] = job;
  r.m.selected = "LCD.GCO";
  r.m.sd_printing = true;
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().job == Job::Printing; }, 5000));
}

// ---- binary transfer (M28 B1) -----------------------------------------------------------

static std::string run_upload(Rig& r, const std::string& name, LineSource* src, uint32_t max_ms,
                              const std::function<void()>& during = nullptr);

#include "binary_transfer.h"
#include "heatshrink_encoder.h"

static FakeMarlin::Options binary_options() {
  FakeMarlin::Options o;
  o.binary = true;
  return o;
}

static bool executed(const Rig& r, const char* prefix) {
  for (auto& e : r.m.executed)
    if (starts_with(e, prefix)) return true;
  return false;
}

static std::string hs_decode(const std::string& c) {
  heatshrink_decoder d;
  heatshrink_decoder_reset(&d);
  std::string out;
  uint8_t buf[512];
  size_t i = 0, n = 0;
  auto drain = [&] {
    HSD_poll_res r;
    do {
      r = heatshrink_decoder_poll(&d, buf, sizeof buf, &n);
      out.append((char*)buf, n);
    } while (r == HSDR_POLL_MORE);
  };
  while (i < c.size()) {
    heatshrink_decoder_sink(&d, (uint8_t*)c.data() + i, c.size() - i, &n);
    i += n;
    drain();
  }
  heatshrink_decoder_finish(&d);
  drain();
  return out;
}

void test_binary_packet_format() {
  // SYNC as the reference client builds it: token AD B5, sync 0, meta 0x01, size 0, and the
  // header checksum over those four bytes.
  TEST_ASSERT_EQUAL_STRING(std::string("\xAD\xB5\x00\x01\x00\x00\x01\x03", 8).c_str(),
                           binary_packet(0, 0, 1).c_str());
  TEST_ASSERT_EQUAL(8, binary_packet(0, 0, 1).size());
  std::string p = binary_packet(7, 1, 3, "abc");
  TEST_ASSERT_EQUAL(8 + 3 + 2, p.size());
  uint16_t cs = fletcher16((const uint8_t*)p.data() + 2, 9);
  TEST_ASSERT_EQUAL_HEX16(cs, (uint8_t)p[11] | ((uint8_t)p[12] << 8));
}

void test_heatshrink_round_trip() {
  // Against Marlin's own decoder, in uneven pieces, text and random bytes.
  std::mt19937 rng(7);
  for (int t = 0; t < 200; t++) {
    std::string in;
    int len = rng() % 3000, alpha = 1 + rng() % 256;
    for (int i = 0; i < len; i++) in += (char)(rng() % alpha);
    if (t % 3 == 0) in = expected_file(1 + t);
    HeatshrinkEncoder e;
    std::string c;
    for (size_t pos = 0; pos < in.size();) {
      size_t k = std::min<size_t>(1 + rng() % 300, in.size() - pos);
      e.write((const uint8_t*)in.data() + pos, k, c);
      pos += k;
    }
    e.finish(c);
    TEST_ASSERT_TRUE_MESSAGE(hs_decode(c) == in, ("case " + std::to_string(t)).c_str());
  }
}

static void binary_upload(FakeMarlin::Options o, double h2p, double p2h, int seconds, int seed = 1) {
  Rig r(o);
  r.m.rng.seed(seed);
  TEST_ASSERT_TRUE(r.connect());
  r.m.h2p_error = h2p;
  r.m.p2h_error = p2h;
  std::string msg = r.upload("TEST.GCO", seconds, 600000);
  r.m.h2p_error = r.m.p2h_error = 0;
  assert_starts("OK: ", msg);
  TEST_ASSERT_TRUE_MESSAGE(msg.find("binary, compressed") != std::string::npos, msg.c_str());
  TEST_ASSERT_TRUE(r.m.files["TEST.GCO"] == expected_file(seconds));
  TEST_ASSERT_TRUE(executed(r, "M28 B1"));
  TEST_ASSERT_FALSE(r.m.binary_mode);
  TEST_ASSERT_EQUAL(0, r.m.bin_aborts);
  for (auto& e : r.m.executed) TEST_ASSERT_FALSE_MESSAGE(starts_with(e, "M117 Halyard"), e.c_str());
  // Numbered lines carry on where they left off.
  std::string again = r.upload("AGAIN.GCO", 2);
  assert_starts("OK: ", again);
  printf("    %s | fake: %d packets, %d bad\n", msg.c_str(), r.m.bin_packets, r.m.bin_bad);
}

// Motion-like lines that compress about as well as real G-code (2-3x), unlike test jobs.
struct RandomGcode : LineSource {
  std::mt19937 rng;
  int left;
  RandomGcode(int lines, int seed) : rng(seed), left(lines) {}
  bool next(std::string& line) override {
    if (left-- <= 0) return false;
    char buf[64];
    snprintf(buf, sizeof buf, "G1 X%.3f Y%.3f E%.5f", (rng() % 200000) / 1000.0,
             (rng() % 200000) / 1000.0, (rng() % 100000) / 100000.0);
    line = buf;
    return true;
  }
  static std::string expected(int lines, int seed) {
    RandomGcode g(lines, seed);
    std::string l, all;
    while (g.next(l)) all += l + "\r\n";
    return all;
  }
};

void test_binary_stress_many_seeds() {
  int failures = 0;
  uint32_t worst_ms = 0;
  for (int seed = 1; seed <= 20; seed++) {
    Rig r(binary_options());
    r.m.rng.seed(seed);
    if (!r.connect()) { failures++; continue; }
    r.m.h2p_error = r.m.p2h_error = 1.0 / 1500;
    uint32_t t0 = r.now;
    std::string msg = run_upload(r, "S.GCO", new RandomGcode(1500, seed), 600000);
    uint32_t ms = r.now - t0;
    worst_ms = std::max(worst_ms, ms);
    bool same = r.m.files["S.GCO"] == RandomGcode::expected(1500, seed);
    if (!starts_with(msg, "OK: ") || !same || r.m.binary_mode) {
      failures++;
      printf("    seed %d: %s, content %s\n", seed, msg.c_str(), same ? "same" : "DIFFERENT");
    } else if (ms > 20000) {
      printf("    seed %d slow (%u ms): %s\n", seed, ms, msg.c_str());
    }
  }
  printf("    20 seeds at 1/1500 both ways, worst %u ms\n", worst_ms);
  TEST_ASSERT_EQUAL(0, failures);
}

void test_binary_throughput() {
  // Clean line, realistic data: what the protocol can do at 250000 baud.
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  uint32_t t0 = r.now;
  std::string msg = run_upload(r, "T.GCO", new RandomGcode(5000, 3), 600000);
  assert_starts("OK: ", msg);
  printf("    %s (%u ms simulated)\n", msg.c_str(), r.now - t0);
  Rig a;
  TEST_ASSERT_TRUE(a.connect());
  a.printer.set_binary(false);
  t0 = a.now;
  msg = run_upload(a, "T.GCO", new RandomGcode(5000, 3), 600000);
  printf("    ascii: %s (%u ms simulated)\n", msg.c_str(), a.now - t0);
}

void test_binary_upload_clean() { binary_upload(binary_options(), 0, 0, 400); }
void test_binary_upload_noisy_to_printer() { binary_upload(binary_options(), 1.0 / 3000, 0, 400); }
void test_binary_upload_noisy_from_printer() { binary_upload(binary_options(), 0, 1.0 / 3000, 400); }
void test_binary_upload_noisy_both_ways() {
  for (int seed = 1; seed <= 10; seed++)
    binary_upload(binary_options(), 1.0 / 2000, 1.0 / 2000, 200, seed);
}

void test_binary_upload_small_rx_buffer() {
  // A stock 128-byte RX buffer holds one packet: a wider window overflows it, and the
  // resend path has to carry the transfer.
  FakeMarlin::Options o = binary_options();
  o.rx_size = 128;
  binary_upload(o, 0, 0, 200);
}

static std::string run_upload(Rig& r, const std::string& name, LineSource* src, uint32_t max_ms,
                              const std::function<void()>& during) {
  bool done = false;
  std::string msg;
  if (!r.printer.upload(name, std::unique_ptr<LineSource>(src), [&](bool ok, const std::string& m2) {
        done = true;
        msg = (ok ? "OK: " : "FAIL: ") + m2;
      }))
    return "FAIL: not started";
  for (uint32_t i = 0; i < max_ms && !done; i++) {
    if (during) during();
    r.step();
  }
  return done ? msg : "FAIL: timed out";
}

void test_binary_abort_deletes_only_its_file() {
  // Marlin's binary ABORT would delete card.filename, which the printer's own file menu
  // overwrites. Someone browses it mid-upload; then the upload is aborted.
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["PRECIOUS.GCO"] = "M117 keep me\r\n";
  std::string msg = run_upload(r, "ABORT.GCO", new TestJobSource(2000), 60000, [&] {
    if (r.m.bin_active && r.m.bin_file.size() > 3000 && r.m.card_filename != "PRECIOUS.GCO") {
      r.m.card_filename = "PRECIOUS.GCO";  // the LCD showed it
      r.printer.abort_upload();
    }
  });
  assert_starts("FAIL: aborted; partial file deleted", msg);
  TEST_ASSERT_EQUAL(0, r.m.files.count("ABORT.GCO"));
  TEST_ASSERT_EQUAL(1, r.m.files.count("PRECIOUS.GCO"));
  TEST_ASSERT_EQUAL(0, r.m.bin_aborts);
  TEST_ASSERT_FALSE(r.m.binary_mode);
  TEST_ASSERT_TRUE(r.m.mounted);
  assert_starts("OK: ", r.upload("NEXT.GCO", 2));
}

void test_binary_open_failure_and_ioerror() {
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  r.m.fail_open = true;
  std::string msg = r.upload("NOPE.GCO", 5);
  assert_starts("FAIL: binary transfer: printer could not open NOPE.GCO", msg);
  TEST_ASSERT_FALSE(r.m.binary_mode);
  r.m.fail_open = false;
  r.m.bin_ioerror = true;
  msg = r.upload("IOERR.GCO", 5);
  assert_starts("FAIL: binary transfer: printer's card: write failed; partial file deleted", msg);
  TEST_ASSERT_EQUAL(0, r.m.files.count("IOERR.GCO"));
  TEST_ASSERT_FALSE(r.m.binary_mode);
  r.m.bin_ioerror = false;
  assert_starts("OK: ", r.upload("FINE.GCO", 5));
}

void test_binary_mode_left_behind_is_closed_by_handshake() {
  // Halyard resets mid-transfer (compressed, so the decoder holds pending bytes): Marlin
  // stays in binary mode and ignores ASCII. The next handshake's SYNC finds it, closes the
  // file transfer while its file is still open, then binary mode. This happened on the
  // printer: closing binary mode first let the handshake's M29 close the file behind the
  // transfer, and from then on every CLOSE failed with PFT:ioerror.
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["PRECIOUS.GCO"] = "M117 keep me\r\n";
  bool done = false;
  r.printer.upload("CUT.GCO", std::unique_ptr<LineSource>(new TestJobSource(2000)),
                   [&](bool, const std::string&) { done = true; });
  TEST_ASSERT_TRUE(r.run_until([&] { return r.m.bin_active && r.m.bin_file.size() > 2000; }, 60000));
  TEST_ASSERT_TRUE(r.m.bin_compress);
  r.m.card_filename = "PRECIOUS.GCO";  // someone browsing the printer's menu
  r.link.reconnect();  // what a fresh boot does
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_FALSE(r.m.binary_mode);
  TEST_ASSERT_FALSE(r.m.bin_active);  // the transfer is closed too, not just binary mode
  TEST_ASSERT_FALSE(r.m.saving);
  bool noted = false;
  for (auto& n : r.notes) noted |= n.find("still in binary mode") != std::string::npos;
  TEST_ASSERT_TRUE(noted);
  r.run(3000);
  bool told = false;
  for (auto& e : r.events) told |= e.find("left open; its partial file stays") != std::string::npos;
  TEST_ASSERT_TRUE(told);
  TEST_ASSERT_EQUAL(1, r.m.files.count("PRECIOUS.GCO"));
  TEST_ASSERT_EQUAL(0, r.m.bin_aborts);
  TEST_ASSERT_TRUE(r.m.files.count("CUT.GCO"));  // the partial stays, under Halyard's name
  assert_starts("OK: ", r.upload("CUT.GCO", 2));  // mounts the card again, overwrites it
  TEST_ASSERT_TRUE(r.m.mounted);
  TEST_ASSERT_TRUE(r.m.files["CUT.GCO"] == expected_file(2));
}

void test_binary_busy_transfer_is_closed_not_aborted() {
  // A transfer left open (binary mode closed, transfer not) meets the next upload's OPEN.
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["PRECIOUS.GCO"] = "M117 keep me\r\n";
  r.m.files["OLD.GCO"] = "";
  r.m.bin_active = true;  // as a cut-off transfer of OLD.GCO leaves it
  r.m.bin_name = "OLD.GCO";
  r.m.card_filename = "PRECIOUS.GCO";
  r.run(3000);
  assert_starts("OK: ", r.upload("NEW.GCO", 3));
  TEST_ASSERT_EQUAL(1, r.m.files.count("PRECIOUS.GCO"));
  TEST_ASSERT_EQUAL(0, r.m.bin_aborts);
  TEST_ASSERT_TRUE(r.m.files["NEW.GCO"] == expected_file(3));
}

void test_binary_off_uses_ascii() {
  Rig r(binary_options());
  TEST_ASSERT_TRUE(r.connect());
  r.printer.set_binary(false);
  std::string msg = r.upload("ASCII.GCO", 5);
  assert_starts("OK: ", msg);
  TEST_ASSERT_TRUE(msg.find("binary") == std::string::npos);
  TEST_ASSERT_FALSE(executed(r, "M28 B1"));
  TEST_ASSERT_TRUE(r.m.files["ASCII.GCO"] == expected_file(5));
}

// ---- commands ---------------------------------------------------------------------------

void test_command_reply_lines() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["A.GCO"] = "G4 P1\r\n";
  bool done = false, ok = false;
  std::vector<FileEntry> files;
  r.printer.list_files([&](bool k, const std::vector<FileEntry>& f) {
    done = true;
    ok = k;
    files = f;
  });
  TEST_ASSERT_TRUE(r.run_until([&] { return done; }, 2000));
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(1, (int)files.size());
  TEST_ASSERT_EQUAL_STRING("A.GCO", files[0].name.c_str());
  TEST_ASSERT_EQUAL_UINT32(7, files[0].size);
  TEST_ASSERT_EQUAL_STRING("0x5B2A6C00", files[0].timestamp.c_str());
  TEST_ASSERT_EQUAL_STRING("A.GCO", files[0].long_name.c_str());
}

void test_unanswered_line_is_retransmitted_then_given_up() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.run(100);
  // The printer takes the line but its ok is lost on the way back.
  r.m.p2h_error = 0;
  bool done = false;
  Outcome got = Outcome::Ok;
  r.link.send("G4 P1", [&](const Reply& rep) {
    done = true;
    got = rep.outcome;
  });
  r.step();  // on the wire
  r.m.killed = true;  // simplest way to lose every reply: the printer goes silent
  TEST_ASSERT_TRUE(r.run_until([&] { return done; }, 20000));
  TEST_ASSERT_EQUAL(Outcome::Lost, got);
  TEST_ASSERT_EQUAL_UINT32(1, r.link.stats().retransmits);
}

// M303 runs for minutes without reading the port, then empties Marlin's RX buffer: lines sent
// behind it would be lost. Without BUSY_WHILE_HEATING its temperature reports are the only
// sign that it is still running.
static void pid_autotune_goes_alone(FakeMarlin::Options o) {
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  r.run(3000);
  std::vector<std::string> order;
  bool finished = false;
  r.link.send("M303 E0 S210 C8 U1", [&](const Reply& rep) {
    order.push_back(std::string("M303 ") + outcome_name(rep.outcome));
    for (auto& l : rep.lines) finished |= l.find("PID Autotune finished") != std::string::npos;
  });
  for (int i = 0; i < 3; i++)
    r.link.send("M117 after " + std::to_string(i), [&](const Reply& rep) {
      order.push_back(std::string("M117 ") + outcome_name(rep.outcome));
    });
  TEST_ASSERT_TRUE(r.run_until([&] { return order.size() == 4; }, 60000));
  TEST_ASSERT_EQUAL_STRING("M303 ok", order[0].c_str());
  for (int i = 1; i < 4; i++) TEST_ASSERT_EQUAL_STRING("M117 ok", order[i].c_str());
  TEST_ASSERT_TRUE(finished);
  TEST_ASSERT_EQUAL_UINT32(0, r.link.stats().retransmits);
  TEST_ASSERT_EQUAL_UINT32(0, r.link.stats().recoveries);
  size_t m117 = 0;
  for (auto& e : r.m.executed) m117 += starts_with(e, "M117 after");
  TEST_ASSERT_EQUAL(3, (int)m117);
}

void test_pid_autotune_goes_alone_full() { pid_autotune_goes_alone({}); }
void test_pid_autotune_goes_alone_without_busy() {
  FakeMarlin::Options o;
  o.busy_while_heating = false;
  pid_autotune_goes_alone(o);
}
void test_pid_autotune_goes_alone_minimal() { pid_autotune_goes_alone(minimal_options()); }

// Replies must reach the command that caused them, even while lines are being resent:
// the ok that follows a Resend belongs to the rejected line, not to the oldest command.
static void replies_stay_attached(FakeMarlin::Options o) {
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  r.run(3000);  // let the setup polls finish
  r.m.files["A.GCO"] = "G4 P1\r\n";
  r.m.h2p_error = 1.0 / 250;
  int done = 0, wrong = 0;
  // A slow command keeps accepted lines waiting in Marlin's queue when an error hits.
  const char* cmds[] = {"M27 C", "G4 P40", "M20"};
  for (int i = 0; i < 150; i++) {
    int kind = i % 3;
    bool sent = r.link.send(cmds[kind], [&, kind](const Reply& rep) {
      done++;
      bool ok = rep.outcome == Outcome::Ok || rep.outcome == Outcome::OkInferred;
      bool mine = kind == 1 ? rep.lines.empty() : false;
      for (auto& l : rep.lines)
        mine |= kind == 2 ? l == "End file list" : kind == 0 && starts_with(l, "Current file");
      if (!ok || !mine) {
        wrong++;
        printf("    %s -> %s:", cmds[kind], outcome_name(rep.outcome));
        for (auto& l : rep.lines) printf(" [%s]", l.c_str());
        printf("\n");
      }
    });
    TEST_ASSERT_TRUE(sent);
  }
  TEST_ASSERT_TRUE(r.run_until([&] { return done == 150; }, 300000));
  printf("    %d recoveries, %d replies not matching their command\n",
         (int)r.link.stats().recoveries, wrong);
  TEST_ASSERT_TRUE(r.link.stats().recoveries > 5);
  TEST_ASSERT_EQUAL(0, wrong);
}

void test_replies_stay_attached_full() { replies_stay_attached({}); }

void test_replies_stay_attached_minimal() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  replies_stay_attached(o);
}

// ---- upload -----------------------------------------------------------------------------

static void upload_and_compare(FakeMarlin::Options o, double h2p, double p2h, int seconds) {
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  r.m.h2p_error = h2p;
  r.m.p2h_error = p2h;
  std::string msg = r.upload("TEST.GCO", seconds, 600000);
  r.m.h2p_error = r.m.p2h_error = 0;
  assert_starts("OK: ", msg);
  TEST_ASSERT_TRUE(r.m.files.count("TEST.GCO"));
  TEST_ASSERT_EQUAL_STRING(expected_file(seconds).c_str(), r.m.files["TEST.GCO"].c_str());
  // Nothing from the file ran as a command.
  for (auto& e : r.m.executed) TEST_ASSERT_FALSE_MESSAGE(starts_with(e, "M117 Halyard"), e.c_str());
  if (h2p > 0) TEST_ASSERT_TRUE(r.link.stats().recoveries > 0);
  const LinkStats& st = r.link.stats();
  printf("    %s | %u lines, %u resend requests, %u recoveries, %u retransmits, %u inferred, "
         "%u lost, %u garbled, %u reconnects, printer errors %d\n",
         msg.c_str(), st.lines_sent, st.resend_requests, st.recoveries, st.retransmits,
         st.inferred, st.lost, st.garbled, st.connects - 1, r.m.errors);
}

// Many seeds at a high error rate, both firmware flavours: looking for livelocks and
// corrupted files rather than for speed.
void test_upload_stress_many_seeds() {
  FakeMarlin::Options minimal;
  minimal.advanced_ok = minimal.emergency_parser = minimal.autoreport_temp = false;
  minimal.autoreport_sd = minimal.long_filename = false;
  int failures = 0;
  for (int seed = 1; seed <= 40; seed++) {
    Rig r(seed % 2 ? FakeMarlin::Options() : minimal);
    r.m.rng.seed(seed);
    if (!r.connect()) { failures++; continue; }
    r.m.h2p_error = r.m.p2h_error = 1.0 / 1500;
    std::string msg = r.upload("S.GCO", 120, 900000);
    bool same = r.m.files["S.GCO"] == expected_file(120);
    if (!starts_with(msg, "OK: ") || !same) {
      failures++;
      printf("    seed %d: %s, content %s\n", seed, msg.c_str(), same ? "same" : "DIFFERENT");
    }
    for (auto& e : r.m.executed)
      if (starts_with(e, "M117 Halyard")) {
        failures++;
        printf("    seed %d: file line ran as a command: %s\n", seed, e.c_str());
        break;
      }
  }
  TEST_ASSERT_EQUAL(0, failures);
}

void test_upload_clean() { upload_and_compare({}, 0, 0, 200); }

void test_upload_noisy_to_printer() { upload_and_compare({}, 1.0 / 3000, 0, 400); }

void test_upload_noisy_from_printer() { upload_and_compare({}, 0, 1.0 / 3000, 400); }

void test_upload_noisy_both_ways() { upload_and_compare({}, 1.0 / 4000, 1.0 / 4000, 400); }

void test_upload_minimal_firmware_noisy() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  upload_and_compare(o, 1.0 / 4000, 1.0 / 4000, 300);
}

void test_upload_small_rx_buffer() {
  FakeMarlin::Options o;
  o.rx_size = 128;
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("TEST.GCO", 200));
  TEST_ASSERT_EQUAL(0, r.m.rx_overflows);
}

void test_upload_refused_while_printing() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["JOB.GCO"] = expected_file(30);
  bool started = false;
  r.printer.start_print("JOB.GCO", [&](bool ok, const std::string&) { started = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return started; }, 3000));
  // The status poll has not caught up yet: the upload's own M27 check must catch it.
  r.printer.poll(r.now);
  std::string msg = r.upload("NEW.GCO", 5);
  if (msg == "FAIL: not started") return;  // refused outright; also fine
  assert_starts("FAIL: printer is printing", msg);
  TEST_ASSERT_TRUE(r.m.sd_printing);
}

// Halyard restarts (an OTA update) while the printer prints a job of 1 s dwells. Its handshake
// waits behind the job's queued commands for seconds. Found on hardware: a fixed 1.5 s timeout
// restarted the handshake forever and switched to another baud rate, sending junk to a
// printing printer. A printer that is talking is busy, not gone: wait for it.
static void reconnect_during_print(FakeMarlin::Options o) {
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  r.m.files["JOB.GCO"] = expected_file(60);
  bool started = false;
  r.printer.start_print("JOB.GCO", [&](bool ok, const std::string&) { started = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return started; }, 3000));
  r.run(3000);
  Link::Config c;  // the restarted Halyard, on the same wire; it remembers the baud rate
  c.known_baud = r.link.caps().baud;
  Link l2(r.t, c);
  Printer p2(l2);
  uint32_t baud = r.m.host_baud;
  bool changed_baud = false;
  for (uint32_t i = 0; i < 30000 && l2.state() != LinkState::Ready; i++) {
    p2.poll(r.now);
    r.m.tick(r.now);
    r.now++;
    changed_baud |= r.m.host_baud != baud;
  }
  TEST_ASSERT_TRUE_MESSAGE(l2.state() == LinkState::Ready, "no handshake during the print");
  TEST_ASSERT_FALSE(changed_baud);
  TEST_ASSERT_TRUE(r.m.sd_printing);
}

void test_reconnect_during_print() { reconnect_during_print({}); }
void test_reconnect_during_print_minimal() { reconnect_during_print(minimal_options()); }

void test_upload_open_failure_sends_nothing_else() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  r.m.fail_open = true;
  std::string msg = r.upload("HALYTEST.GCO", 5);
  assert_starts("FAIL: M28 failed", msg);
  for (auto& e : r.m.executed) TEST_ASSERT_FALSE_MESSAGE(starts_with(e, "M117 Halyard"), e.c_str());
}

void test_upload_rejects_unsafe_line() {
  struct Bad : LineSource {
    int i = 0;
    bool next(std::string& l) override {
      if (i++ >= 3) return false;
      l = i == 2 ? "M117 50*off" : "G4 P1";
      return true;
    }
  };
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  bool done = false;
  std::string msg;
  r.printer.upload("BAD.GCO", std::unique_ptr<LineSource>(new Bad()),
                   [&](bool ok, const std::string& m2) {
                     done = true;
                     msg = (ok ? "OK: " : "FAIL: ") + m2;
                   });
  TEST_ASSERT_TRUE(r.run_until([&] { return done; }, 10000));
  assert_starts("FAIL: line 2", msg);
  TEST_ASSERT_FALSE(r.m.files.count("BAD.GCO"));  // partial file deleted
}

void test_reconnect_mid_upload_closes_file() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  bool done = false;
  std::string msg;
  r.printer.upload("MID.GCO", std::unique_ptr<LineSource>(new TestJobSource(500)),
                   [&](bool ok, const std::string& m2) {
                     done = true;
                     msg = m2;
                   });
  TEST_ASSERT_TRUE(r.run_until([&] { return r.m.saving && r.m.save_data.size() > 2000; }, 20000));
  r.link.reconnect();  // e.g. an unrecoverable resend; the printer is still saving
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_FALSE(r.m.saving);
  // The handshake ran as commands, not as file content.
  TEST_ASSERT_TRUE(r.link.caps().has("SD_WRITE"));
  TEST_ASSERT_EQUAL(std::string::npos, r.m.files["MID.GCO"].find("M115"));
}

// ---- printing ---------------------------------------------------------------------------

static void print_progress_cancel(FakeMarlin::Options o) {
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("JOB.GCO", 60));
  bool started = false;
  r.printer.start_print("JOB.GCO", [&](bool ok, const std::string&) { started = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return started; }, 5000));
  auto progress_events = [&] {
    size_t n = 0;
    for (auto& e : r.events) n += starts_with(e, "progress ");
    return n;
  };
  if (!r.run_until([&] { return progress_events() >= 2; }, 20000)) {
    for (auto& e : r.events) printf("event: %s\n", e.c_str());
    for (auto& n : r.notes) printf("note: %s\n", n.c_str());
    printf("job=%s sd_pos=%u marlin sd_pos=%zu queue=%zu inflight=%zu\n",
           job_name(r.printer.status().job), r.printer.status().sd_pos, r.m.sd_pos,
           r.link.queued(), r.link.unanswered());
    TEST_FAIL_MESSAGE("no progress reports");
  }

  bool cancelled = false;
  std::string cmsg;
  r.printer.cancel([&](bool ok, const std::string& m2) {
    cancelled = ok;
    cmsg = m2;
  });
  TEST_ASSERT_TRUE_MESSAGE(r.run_until([&] { return cancelled; }, 30000), cmsg.c_str());
  TEST_ASSERT_FALSE(r.m.sd_printing);
  TEST_ASSERT_EQUAL(Job::Idle, r.printer.status().job);
  TEST_ASSERT_EQUAL(1, r.m.homes);  // the firmware's EVENT_GCODE_SD_ABORT

  // The link is still in step afterwards.
  bool listed = false;
  r.printer.list_files([&](bool ok, const std::vector<FileEntry>&) { listed = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return listed; }, 15000));
}

void test_print_progress_cancel_full() { print_progress_cancel({}); }

void test_print_progress_cancel_minimal() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  print_progress_cancel(o);
}

void test_print_runs_to_completion() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("SHORT.GCO", 3));
  bool started = false;
  r.printer.start_print("SHORT.GCO", [&](bool ok, const std::string&) { started = ok; });
  TEST_ASSERT_TRUE(r.run_until([&] { return started; }, 5000));
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().job == Job::Idle; }, 15000));
  bool finished = false;
  for (auto& e : r.events) finished |= e == "print finished";
  TEST_ASSERT_TRUE(finished);
  TEST_ASSERT_EQUAL(0, r.m.homes);
}

void test_pause_resume() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("JOB.GCO", 30));
  bool ok = false;
  r.printer.start_print("JOB.GCO", [&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  ok = false;
  r.printer.pause([&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  r.run(3000);
  uint32_t pos = r.m.sd_pos;
  r.run(3000);
  TEST_ASSERT_EQUAL_UINT32(pos, r.m.sd_pos);
  ok = false;
  r.printer.resume([&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  TEST_ASSERT_TRUE(r.run_until([&] { return r.m.sd_pos > pos; }, 5000));
}

// A pause from the printer's own menu (M25 from MarlinUI): M27 still says "SD printing
// byte", so only the host action tells. So does the resume (M24) from there.
void test_pause_resume_on_printer_screen() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("JOB.GCO", 30));
  bool ok = false;
  r.printer.start_print("JOB.GCO", [&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  r.run(2000);
  TEST_ASSERT_EQUAL(Job::Printing, r.printer.status().job);
  r.m.queue.push_back({"M25", false});
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().job == Job::Paused; }, 5000));
  r.run(6000);  // several M27 reports, all at the same position
  TEST_ASSERT_EQUAL(Job::Paused, r.printer.status().job);
  r.m.queue.push_back({"M24", false});
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().job == Job::Printing; }, 5000));
}

// Without host actions, a pause from Halyard still ends when the printer's own screen
// resumes the print: the position M27 reports moves again.
static void resume_on_printer_screen_unannounced(FakeMarlin::Options o) {
  o.host_actions = false;
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  assert_starts("OK: ", r.upload("JOB.GCO", 30));
  bool ok = false;
  r.printer.start_print("JOB.GCO", [&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  r.run(2000);
  ok = false;
  r.printer.pause([&](bool k, const std::string&) { ok = k; });
  TEST_ASSERT_TRUE(r.run_until([&] { return ok; }, 5000));
  r.run(8000);
  TEST_ASSERT_EQUAL(Job::Paused, r.printer.status().job);
  r.m.queue.push_back({"M24", false});
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().job == Job::Printing; }, 8000));
}

void test_resume_on_printer_screen_unannounced_full() { resume_on_printer_screen_unannounced({}); }

void test_resume_on_printer_screen_unannounced_minimal() {
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  resume_on_printer_screen_unannounced(o);
}

// The heaters' power comes with the temperatures: the soft PWM, 0 to 127.
static void heater_power(FakeMarlin::Options o) {
  Rig r(o);
  r.m.hotend_power = 127;
  r.m.bed_power = 64;
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().hotend_power > 0; }, 5000));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1.0, r.printer.status().hotend_power);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 0.5, r.printer.status().bed_power);
  r.m.hotend_power = 0;
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().hotend_power == 0; }, 5000));
}

void test_heater_power_full() { heater_power({}); }

void test_heater_power_minimal() {  // from M105's "ok T:..."
  FakeMarlin::Options o;
  o.advanced_ok = o.emergency_parser = o.autoreport_temp = o.autoreport_sd = o.long_filename = false;
  heater_power(o);
}

// A command Marlin doesn't know is answered at once, without ADVANCED_OK too: the
// "Unknown command" line names it, so its ok is ours. Found with the stock profile: the
// name was read one character short, the ok was dropped, and every unknown command (M900
// at each connect, without LIN_ADVANCE) stalled the link for 10 s.
static void unknown_command_answered(FakeMarlin::Options o) {
  o.lin_advance = false;
  Rig r(o);
  TEST_ASSERT_TRUE(r.connect());
  r.run(1000);
  bool done = false;
  Reply got;
  r.link.send("M900", [&](const Reply& rep) { done = true; got = rep; });
  TEST_ASSERT_TRUE(r.run_until([&] { return done; }, 1000));
  TEST_ASSERT_EQUAL(Outcome::Ok, got.outcome);
  TEST_ASSERT_EQUAL(1, (int)got.lines.size());
  TEST_ASSERT_EQUAL_STRING("echo:Unknown command: \"M900\"", got.lines[0].c_str());
  for (auto& e : r.events) TEST_ASSERT_TRUE(e.find("not answering") == std::string::npos);
}

void test_unknown_command_answered_full() { unknown_command_answered({}); }
void test_unknown_command_answered_stock() { unknown_command_answered(FakeMarlin::stock()); }
void test_unknown_command_answered_minimal() { unknown_command_answered(minimal_options()); }

// Stock Marlin 2.1.2.8 (FakeMarlin::stock(): its own defaults plus SD support), next to the
// full-featured one and the minimal one.
void test_handshake_stock() {
  Rig r(FakeMarlin::stock());
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_FALSE(r.link.caps().advanced_ok);
  TEST_ASSERT_TRUE(r.link.caps().has("AUTOREPORT_TEMP"));
  TEST_ASSERT_FALSE(r.link.caps().has("EEPROM"));
  TEST_ASSERT_FALSE(r.link.caps().has("BINARY_FILE_TRANSFER"));
  TEST_ASSERT_TRUE(r.run_until([&] { return r.printer.status().temps_valid; }, 5000));
}
void test_upload_stock_noisy() { upload_and_compare(FakeMarlin::stock(), 1.0 / 4000, 1.0 / 4000, 300); }
void test_pid_autotune_goes_alone_stock() { pid_autotune_goes_alone(FakeMarlin::stock()); }
void test_replies_stay_attached_stock() { replies_stay_attached(FakeMarlin::stock()); }
void test_reconnect_during_print_stock() { reconnect_during_print(FakeMarlin::stock()); }
void test_print_progress_cancel_stock() { print_progress_cancel(FakeMarlin::stock()); }
void test_resume_on_printer_screen_unannounced_stock() {
  resume_on_printer_screen_unannounced(FakeMarlin::stock());
}
void test_heater_power_stock() { heater_power(FakeMarlin::stock()); }

void test_emergency_stop_halts_link() {
  Rig r;
  TEST_ASSERT_TRUE(r.connect());
  TEST_ASSERT_TRUE(r.link.send_now("M112"));
  TEST_ASSERT_TRUE(r.run_until([&] { return r.link.state() == LinkState::Halted; }, 2000));
  r.m.reboot();
  TEST_ASSERT_TRUE(r.connect());
}

void setUp();
void tearDown();

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checksum_and_numbering);
  RUN_TEST(test_sanitize);
  RUN_TEST(test_handshake_full_features);
  RUN_TEST(test_handshake_minimal_firmware);
  RUN_TEST(test_handshake_finds_baud);
  RUN_TEST(test_handshake_through_boot_junk);
  RUN_TEST(test_reconnects_after_printer_reset);
  RUN_TEST(test_reconnects_when_idle_printer_returns_at_other_baud);
  RUN_TEST(test_no_reconnect_when_printer_goes_quiet_mid_print);
  RUN_TEST(test_notices_print_started_on_printer);
  RUN_TEST(test_command_reply_lines);
  RUN_TEST(test_unanswered_line_is_retransmitted_then_given_up);
  RUN_TEST(test_pid_autotune_goes_alone_full);
  RUN_TEST(test_pid_autotune_goes_alone_without_busy);
  RUN_TEST(test_pid_autotune_goes_alone_minimal);
  RUN_TEST(test_replies_stay_attached_full);
  RUN_TEST(test_replies_stay_attached_minimal);
  RUN_TEST(test_upload_clean);
  RUN_TEST(test_upload_noisy_to_printer);
  RUN_TEST(test_upload_noisy_from_printer);
  RUN_TEST(test_upload_noisy_both_ways);
  RUN_TEST(test_upload_minimal_firmware_noisy);
  RUN_TEST(test_upload_small_rx_buffer);
  RUN_TEST(test_upload_stress_many_seeds);
  RUN_TEST(test_upload_refused_while_printing);
  RUN_TEST(test_upload_open_failure_sends_nothing_else);
  RUN_TEST(test_reconnect_during_print);
  RUN_TEST(test_reconnect_during_print_minimal);
  RUN_TEST(test_upload_rejects_unsafe_line);
  RUN_TEST(test_reconnect_mid_upload_closes_file);
  RUN_TEST(test_print_progress_cancel_full);
  RUN_TEST(test_print_progress_cancel_minimal);
  RUN_TEST(test_print_runs_to_completion);
  RUN_TEST(test_pause_resume);
  RUN_TEST(test_pause_resume_on_printer_screen);
  RUN_TEST(test_resume_on_printer_screen_unannounced_full);
  RUN_TEST(test_resume_on_printer_screen_unannounced_minimal);
  RUN_TEST(test_heater_power_full);
  RUN_TEST(test_heater_power_minimal);
  RUN_TEST(test_unknown_command_answered_full);
  RUN_TEST(test_unknown_command_answered_stock);
  RUN_TEST(test_unknown_command_answered_minimal);
  RUN_TEST(test_handshake_stock);
  RUN_TEST(test_upload_stock_noisy);
  RUN_TEST(test_pid_autotune_goes_alone_stock);
  RUN_TEST(test_replies_stay_attached_stock);
  RUN_TEST(test_reconnect_during_print_stock);
  RUN_TEST(test_print_progress_cancel_stock);
  RUN_TEST(test_resume_on_printer_screen_unannounced_stock);
  RUN_TEST(test_heater_power_stock);
  RUN_TEST(test_emergency_stop_halts_link);
  RUN_TEST(test_binary_packet_format);
  RUN_TEST(test_heatshrink_round_trip);
  RUN_TEST(test_binary_upload_clean);
  RUN_TEST(test_binary_upload_noisy_to_printer);
  RUN_TEST(test_binary_upload_noisy_from_printer);
  RUN_TEST(test_binary_upload_noisy_both_ways);
  RUN_TEST(test_binary_upload_small_rx_buffer);
  RUN_TEST(test_binary_stress_many_seeds);
  RUN_TEST(test_binary_throughput);
  RUN_TEST(test_binary_abort_deletes_only_its_file);
  RUN_TEST(test_binary_busy_transfer_is_closed_not_aborted);
  RUN_TEST(test_binary_open_failure_and_ioerror);
  RUN_TEST(test_binary_mode_left_behind_is_closed_by_handshake);
  RUN_TEST(test_binary_off_uses_ascii);
  return UNITY_END();
}

void setUp() {}
void tearDown() {}
