// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <cctype>
#include <cstring>

#include "access.h"

namespace halyard {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// The text split into lines without their endings; a UTF-8 BOM (older Notepad) is dropped.
std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string::npos ? text.size() : nl;
    std::string l = text.substr(pos, end - pos);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
    pos = end + 1;
  }
  return out;
}

bool is_comment(const std::string& t) { return t.empty() || t[0] == '#' || t[0] == ';'; }

// "[wifi]" -> "wifi"; false if the line is not a section header.
bool section_of(const std::string& t, std::string& name) {
  if (t.size() < 2 || t[0] != '[' || t.back() != ']') return false;
  name = lower(trim(t.substr(1, t.size() - 2)));
  return true;
}

// "key: value" or "key = value"; false if there is no separator.
bool key_of(const std::string& t, std::string& key, std::string& value) {
  size_t sep = t.find_first_of(":=");
  if (sep == std::string::npos || sep == 0) return false;
  key = lower(trim(t.substr(0, sep)));
  value = t.substr(sep + 1);
  return !key.empty();
}

// Klipper's inline comments: '#' or ';' after whitespace.
std::string strip_inline_comment(const std::string& v) {
  for (size_t i = 1; i < v.size(); i++)
    if ((v[i] == '#' || v[i] == ';') && isspace((unsigned char)v[i - 1])) return v.substr(0, i);
  return v;
}

bool parse_bool(const std::string& v, bool& out) {
  std::string l = lower(v);
  if (l == "on" || l == "yes" || l == "true" || l == "1" || l == "auto") return out = true, true;
  if (l == "off" || l == "no" || l == "false" || l == "0") return out = false, true;
  return false;
}

bool valid_hostname(const std::string& h) {
  if (h.empty() || h.size() > 32 || h.front() == '-' || h.back() == '-') return false;
  for (unsigned char c : h)
    if (!isalnum(c) && c != '-') return false;
  return true;
}

bool valid_api_key(const std::string& k) {
  if (k.size() < 16 || k.size() > 64) return false;
  for (unsigned char c : k)
    if (!isalnum(c) && c != '-' && c != '_') return false;
  return true;
}

}  // namespace

Settings::Settings() : trusted(Access::kDefaultTrusted) {}

Settings parse_settings(const std::string& text, const Settings& defaults,
                        std::vector<std::string>& warnings) {
  Settings s = defaults;
  std::string section;
  std::vector<std::string> seen;
  int n = 0;
  for (const std::string& raw : lines_of(text)) {
    n++;
    std::string where = "halyard.cfg line " + std::to_string(n) + ": ";
    std::string t = trim(raw);
    if (is_comment(t)) continue;
    std::string name;
    if (section_of(t, name)) {
      section = name;
      if (section != "wifi" && section != "access" && section != "printer" && section != "updates")
        warnings.push_back(where + "unknown section [" + name + "]");
      continue;
    }
    std::string key, value;
    if (!key_of(t, key, value)) {
      warnings.push_back(where + "expected \"name: value\", got \"" + t + "\"");
      continue;
    }
    if (section.empty()) {
      warnings.push_back(where + key + " is outside any [section]");
      continue;
    }
    std::string id = section + "." + key;
    bool whole_line = id == "wifi.ssid" || id == "wifi.password";
    value = trim(whole_line ? value : strip_inline_comment(value));
    for (auto& k : seen)
      if (k == id) warnings.push_back(where + key + " is set twice in [" + section + "]; the last one counts");
    seen.push_back(id);

    std::string bad;
    if (id == "wifi.ssid") {
      if (value.size() > 32) bad = "a WiFi name has at most 32 characters";
      else s.ssid = value;
    } else if (id == "wifi.password") {
      if (!value.empty() && (value.size() < 8 || value.size() > 64))
        bad = "a WiFi password has 8 to 64 characters (or none, for an open network)";
      else s.password = value;
    } else if (id == "wifi.hostname") {
      std::string h = lower(value);
      if (!valid_hostname(h)) bad = "use letters, digits and '-', at most 32";
      else s.hostname = h;
    } else if (id == "access.trusted") {
      Access a;
      std::string err, list = lower(value) == "none" ? "" : value;
      if (!a.set_trusted(list, err)) bad = err;
      else s.trusted = a.trusted();
    } else if (id == "access.api_key") {
      if (!value.empty() && !valid_api_key(value))
        bad = "16 to 64 letters, digits, '-' or '_' (or leave it empty for a new one)";
      else s.api_key = value;
    } else if (id == "printer.binary_transfer") {
      if (!parse_bool(value, s.binary_transfer)) bad = "use auto or off";
    } else if (id == "printer.screen_messages") {
      if (!parse_bool(value, s.screen_messages)) bad = "use on or off";
    } else if (id == "updates.source") {
      std::string l = lower(value);
      if (!value.empty() && ((l.compare(0, 7, "http://") != 0 && l.compare(0, 8, "https://") != 0) ||
                             value.find(' ') != std::string::npos || value.size() > 200))
        bad = "a folder's address, http://... or https://... (or leave it empty)";
      else s.update_source = value;
    } else {
      warnings.push_back(where + "unknown option " + key + " in [" + section + "]");
      continue;
    }
    if (!bad.empty()) warnings.push_back(where + key + ": " + bad + "; ignored");
  }
  return s;
}

// "key: value", without a trailing space when the value is empty.
static std::string kv(const char* key, const std::string& value) {
  return std::string(key) + ":" + (value.empty() ? "" : " " + value) + "\n";
}

std::string settings_file(const Settings& s) {
  std::string t;
  t += "# Halyard's settings. Edit them in Mainsail (Machine -> halyard.cfg, then SAVE & RESTART),\n";
  t += "# or on a PC with the card in a card reader. Lines starting with # are comments.\n";
  t += "\n";
  t += "[wifi]\n";
  t += "# The network Halyard joins. The name and password take the whole rest of the line.\n";
  t += kv("ssid", s.ssid);
  t += kv("password", s.password);
  t += "# Halyard's name on the network: http://NAME.local/ (letters, digits and -).\n";
  t += kv("hostname", s.hostname);
  t += "\n";
  t += "[access]\n";
  t += "# Addresses that need no API key. Mainsail has no login, so your browser must be in\n";
  t += "# one of these. The default is every private (home network) range. \"none\" trusts\n";
  t += "# nobody.\n";
  t += kv("trusted", s.trusted.empty() ? "none" : s.trusted);
  t += "# Slicers outside the trusted ranges send this as their API key. Halyard fills it in;\n";
  t += "# empty it and restart for a new one.\n";
  t += kv("api_key", s.api_key);
  t += "\n";
  t += "[printer]\n";
  t += "# Marlin's binary file transfer, when the printer offers it: about 3x faster. auto or off.\n";
  t += std::string("binary_transfer: ") + (s.binary_transfer ? "auto" : "off") + "\n";
  t += "# Show Halyard's address, and problems such as a WiFi network it cannot join, on the\n";
  t += "# printer's screen. on or off.\n";
  t += std::string("screen_messages: ") + (s.screen_messages ? "on" : "off") + "\n";
  t += "\n";
  t += "[updates]\n";
  t += "# Where Halyard gets Mainsail when the card has none, and both rows of Mainsail's Update\n";
  t += "# Manager: a folder holding mainsail-<version>.zip and a release's SHA256SUMS and\n";
  t += "# firmware.bin (./run.sh files serves one). Empty: Halyard's own releases.\n";
  t += kv("source", s.update_source);
  return t;
}

void set_setting(std::string& text, const std::string& section, const std::string& key,
                 const std::string& value) {
  std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines = lines_of(text);
  bool bom = text.compare(0, 3, "\xEF\xBB\xBF") == 0;
  std::string want = lower(section), line = kv(key.c_str(), value);
  line.pop_back();  // kv's newline
  bool in = false, found_section = false;
  size_t insert_at = lines.size();
  for (size_t i = 0; i < lines.size(); i++) {
    std::string t = trim(lines[i]), name, k, v;
    if (section_of(t, name)) {
      if (in) break;  // the end of our section
      in = name == want;
      if (in) found_section = true, insert_at = i + 1;
      continue;
    }
    if (!in) continue;
    if (!is_comment(t) && key_of(t, k, v) && k == lower(key)) {
      lines[i] = line;
      insert_at = std::string::npos;
      break;
    }
    if (!t.empty()) insert_at = i + 1;  // after the section's last line, before blank ones
  }
  if (insert_at != std::string::npos) {
    if (found_section) {
      lines.insert(lines.begin() + insert_at, line);
    } else {
      if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
      lines.push_back("[" + section + "]");
      lines.push_back(line);
    }
  }
  text = bom ? "\xEF\xBB\xBF" : "";
  for (auto& l : lines) text += l + nl;
}

bool find_setting(const std::string& text, const std::string& section, const std::string& key,
                  std::string& value) {
  std::string in, want = lower(section);
  for (const std::string& raw : lines_of(text)) {
    std::string t = trim(raw), name, k, v;
    if (section_of(t, name)) {
      in = name;
    } else if (in == want && !is_comment(t) && key_of(t, k, v) && k == lower(key)) {
      bool whole_line = want == "wifi" && (k == "ssid" || k == "password");
      value = trim(whole_line ? v : strip_inline_comment(v));
      return true;
    }
  }
  return false;
}

const char* const kMaskedPassword = "********";

std::string mask_password(const std::string& text) {
  std::string pw, out = text;
  if (find_setting(text, "wifi", "password", pw) && !pw.empty())
    set_setting(out, "wifi", "password", kMaskedPassword);
  return out;
}

std::string unmask_password(const std::string& saved, const std::string& current) {
  std::string pw, old, out = saved;
  if (find_setting(saved, "wifi", "password", pw) && pw == kMaskedPassword) {
    if (!find_setting(current, "wifi", "password", old)) old.clear();
    set_setting(out, "wifi", "password", old);
  }
  return out;
}

}  // namespace halyard
