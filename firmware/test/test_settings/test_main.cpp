// SPDX-License-Identifier: GPL-3.0-or-later
// halyard.cfg: parsing, the file Halyard writes, and editing one value in place.
#include <unity.h>

#include <cstdio>

#include "access.h"
#include "settings.h"

using namespace halyard;

void setUp() {}
void tearDown() {}

static Settings parse(const std::string& text, std::vector<std::string>& w) {
  return parse_settings(text, Settings(), w);
}

static bool has(const std::vector<std::string>& w, const std::string& part) {
  for (auto& l : w)
    if (l.find(part) != std::string::npos) return true;
  return false;
}

void test_the_written_file_reads_back_the_same() {
  Settings s;
  s.ssid = "Home # 5";
  s.password = "pass word;#1";
  s.hostname = "ender3";
  s.trusted = "192.168.1.0/24";
  s.api_key = "0123456789abcdef0123456789abcdef";
  s.binary_transfer = false;
  s.screen_messages = false;
  s.update_source = "http://192.168.1.20:8000/release/";
  std::vector<std::string> w;
  Settings r = parse(settings_file(s), w);
  TEST_ASSERT_EQUAL(0, w.size());
  TEST_ASSERT_EQUAL_STRING("Home # 5", r.ssid.c_str());  // the whole line: '#' is not a comment
  TEST_ASSERT_EQUAL_STRING("pass word;#1", r.password.c_str());
  TEST_ASSERT_EQUAL_STRING("ender3", r.hostname.c_str());
  TEST_ASSERT_EQUAL_STRING("192.168.1.0/24", r.trusted.c_str());
  TEST_ASSERT_EQUAL_STRING(s.api_key.c_str(), r.api_key.c_str());
  TEST_ASSERT_FALSE(r.binary_transfer);
  TEST_ASSERT_FALSE(r.screen_messages);
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8000/release/", r.update_source.c_str());
  // The defaults, as a card prepared on a PC has them: nothing to warn about.
  w.clear();
  r = parse(settings_file(Settings()), w);
  TEST_ASSERT_EQUAL(0, w.size());
  TEST_ASSERT_EQUAL_STRING(Access::kDefaultTrusted, r.trusted.c_str());
  TEST_ASSERT_TRUE(r.ssid.empty());
  TEST_ASSERT_TRUE(r.binary_transfer);
}

void test_windows_notepad_and_klipper_habits() {
  // A BOM, CRLF, "=" instead of ":", upper case, and inline comments after a value.
  std::string text =
      "\xEF\xBB\xBF# made in Notepad\r\n[WiFi]\r\nSSID = Cabin\r\n\r\n[printer]\r\n"
      "binary_transfer: off   # the printer's firmware is odd\r\nscreen_messages: No ; quiet\r\n";
  std::vector<std::string> w;
  Settings r = parse(text, w);
  TEST_ASSERT_EQUAL_MESSAGE(0, w.size(), w.empty() ? "" : w[0].c_str());
  TEST_ASSERT_EQUAL_STRING("Cabin", r.ssid.c_str());
  TEST_ASSERT_FALSE(r.binary_transfer);
  TEST_ASSERT_FALSE(r.screen_messages);
  TEST_ASSERT_EQUAL_STRING("halyard", r.hostname.c_str());  // left out: the default
}

void test_mistakes_warn_and_keep_the_default() {
  std::string text =
      "ssid: stray\n"                  // 1: before any section
      "[wifi]\n"
      "password: short\n"              // 3
      "hostname: my_printer\n"         // 4
      "hostname: second\n"             // 5: set twice
      "[access]\n"
      "trusted: 192.168.1.0/33\n"      // 7
      "api_key: has spaces in it ok\n" // 8
      "[printer]\n"
      "binary_transfer: sometimes\n"   // 10
      "colour: red\n"                  // 11
      "just some words\n"              // 12
      "[klipper]\n"                    // 13
      "[updates]\n"
      "source: ftp://pc/halyard/\n";   // 15
  std::vector<std::string> w;
  Settings r = parse(text, w);
  TEST_ASSERT_TRUE(has(w, "line 1: ssid is outside any [section]"));
  TEST_ASSERT_TRUE(has(w, "line 3: password: a WiFi password has 8 to 64"));
  TEST_ASSERT_TRUE(has(w, "line 4: hostname"));
  TEST_ASSERT_TRUE(has(w, "line 5: hostname is set twice"));
  TEST_ASSERT_TRUE(has(w, "line 7: trusted"));
  TEST_ASSERT_TRUE(has(w, "line 8: api_key"));
  TEST_ASSERT_TRUE(has(w, "line 10: binary_transfer"));
  TEST_ASSERT_TRUE(has(w, "line 11: unknown option colour in [printer]"));
  TEST_ASSERT_TRUE(has(w, "line 12: expected"));
  TEST_ASSERT_TRUE(has(w, "line 13: unknown section [klipper]"));
  TEST_ASSERT_TRUE(has(w, "line 15: source: a folder's address"));
  TEST_ASSERT_EQUAL(11, w.size());
  TEST_ASSERT_TRUE(r.update_source.empty());
  TEST_ASSERT_TRUE(r.ssid.empty());
  TEST_ASSERT_TRUE(r.password.empty());
  TEST_ASSERT_EQUAL_STRING("second", r.hostname.c_str());
  TEST_ASSERT_EQUAL_STRING(Access::kDefaultTrusted, r.trusted.c_str());
  TEST_ASSERT_TRUE(r.api_key.empty());
  TEST_ASSERT_TRUE(r.binary_transfer);
  // "none" trusts nobody; an open network has no password.
  w.clear();
  r = parse("[access]\ntrusted: none\n[wifi]\nssid: Cafe\npassword:\n", w);
  TEST_ASSERT_EQUAL(0, w.size());
  TEST_ASSERT_TRUE(r.trusted.empty());
  TEST_ASSERT_TRUE(r.password.empty());
}

void test_set_setting_keeps_the_rest() {
  std::string text = settings_file(Settings());
  set_setting(text, "access", "api_key", "0123456789abcdef0123456789abcdef");
  set_setting(text, "wifi", "ssid", "Home");
  std::vector<std::string> w;
  Settings r = parse(text, w);
  TEST_ASSERT_EQUAL(0, w.size());
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", r.api_key.c_str());
  TEST_ASSERT_EQUAL_STRING("Home", r.ssid.c_str());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("# Slicers outside the trusted ranges"));

  // A missing key goes at the end of its section, a missing section at the end.
  std::string mine = "[wifi]\r\nssid: A # not a comment\r\n\r\n[printer]\r\nscreen_messages: off\r\n";
  set_setting(mine, "wifi", "password", "12345678");
  set_setting(mine, "access", "trusted", "10.0.0.0/8");
  TEST_ASSERT_EQUAL_STRING(
      "[wifi]\r\nssid: A # not a comment\r\npassword: 12345678\r\n\r\n[printer]\r\n"
      "screen_messages: off\r\n\r\n[access]\r\ntrusted: 10.0.0.0/8\r\n",
      mine.c_str());
  // A commented-out key is left alone.
  std::string c = "[wifi]\n# ssid: old\n";
  set_setting(c, "wifi", "ssid", "new");
  TEST_ASSERT_EQUAL_STRING("[wifi]\n# ssid: old\nssid: new\n", c.c_str());
}

// card/halyard.cfg is what a card prepared on a PC starts with (tools/make_card.py).
// It must be the file Halyard itself would write.
void test_the_card_template_is_the_default_file() {
  std::string path = __FILE__;
  path = path.substr(0, path.rfind('/')) + "/../../../card/halyard.cfg";
  FILE* f = fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string text;
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  fclose(f);
  TEST_ASSERT_EQUAL_STRING(settings_file(Settings()).c_str(), text.c_str());
}

void test_the_password_is_masked_over_the_network() {
  Settings s;
  s.ssid = "Home";
  s.password = "secret #1 pass";
  std::string card = settings_file(s);
  std::string shown = mask_password(card);
  TEST_ASSERT_EQUAL(std::string::npos, shown.find("secret"));
  std::string v;
  TEST_ASSERT_TRUE(find_setting(shown, "wifi", "password", v));
  TEST_ASSERT_EQUAL_STRING(kMaskedPassword, v.c_str());
  // Saved back unchanged, or with other edits: the password on the card stays.
  std::string edited = shown;
  set_setting(edited, "wifi", "hostname", "ender");
  std::string saved = unmask_password(edited, card);
  std::vector<std::string> w;
  Settings r = parse_settings(saved, Settings(), w);
  TEST_ASSERT_EQUAL_STRING("secret #1 pass", r.password.c_str());
  TEST_ASSERT_EQUAL_STRING("ender", r.hostname.c_str());
  // A new one typed over the stars replaces it; an emptied line means an open network.
  std::string typed = shown;
  set_setting(typed, "wifi", "password", "new password");
  TEST_ASSERT_EQUAL_STRING("new password", parse_settings(unmask_password(typed, card), Settings(), w).password.c_str());
  std::string open = shown;
  set_setting(open, "wifi", "password", "");
  TEST_ASSERT_TRUE(parse_settings(unmask_password(open, card), Settings(), w).password.empty());
  // An open network has nothing to hide.
  TEST_ASSERT_EQUAL_STRING(settings_file(Settings()).c_str(), mask_password(settings_file(Settings())).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_written_file_reads_back_the_same);
  RUN_TEST(test_windows_notepad_and_klipper_habits);
  RUN_TEST(test_mistakes_warn_and_keep_the_default);
  RUN_TEST(test_set_setting_keeps_the_rest);
  RUN_TEST(test_the_card_template_is_the_default_file);
  RUN_TEST(test_the_password_is_masked_over_the_network);
  return UNITY_END();
}
