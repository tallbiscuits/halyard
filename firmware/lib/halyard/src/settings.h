// SPDX-License-Identifier: GPL-3.0-or-later
// halyard.cfg: Halyard's settings, in Klipper's format, so Mainsail edits it like printer.cfg
// (Machine -> halyard.cfg, then SAVE & RESTART). It lives at the root of Halyard's card, where
// it can also be written on a PC before the card goes in: that is how a new Halyard learns
// its WiFi network without the USB console.
//
//   [section]
//   key: value        (or key = value)
//   # a comment       (a line starting with # or ;, or " #" after a value)
//
// The WiFi name and password take the whole rest of the line, so they may contain '#'.
// Unknown or bad lines become warnings for Mainsail's notification bell, and the setting keeps
// its default. Portable C++.
#pragma once
#include <string>
#include <vector>

namespace halyard {

struct Settings {
  // [wifi]
  std::string ssid, password;
  std::string hostname = "halyard";  // NAME.local, and the name in the router's list
  // [access]
  std::string trusted;  // as Access::set_trusted takes it; "" trusts nobody
  std::string api_key;  // empty: Halyard makes one and writes it into the file
  // [printer]
  bool binary_transfer = true;  // M28 B1 when the printer offers it
  bool screen_messages = true;  // Halyard's address and problems on the printer's screen
  // [updates]
  // Where Halyard's release files are (a folder URL): Mainsail comes from
  // SOURCE/mainsail-<version>.zip. Empty: Halyard's releases, once there are any.
  std::string update_source;

  Settings();  // trusted = Access::kDefaultTrusted
  bool same_network(const Settings& o) const {
    return ssid == o.ssid && password == o.password && hostname == o.hostname;
  }
};

// Reads the file's text over `defaults`: a setting the file leaves out, or gets wrong, keeps
// its value from there. Each problem adds a line to `warnings`.
Settings parse_settings(const std::string& text, const Settings& defaults,
                        std::vector<std::string>& warnings);

// The whole file, as Halyard writes it when the card has none, with every setting explained.
std::string settings_file(const Settings& s);

// Sets one value in the file's text and keeps everything else, comments included. Adds the
// key to its section, or the section at the end, if they are missing.
void set_setting(std::string& text, const std::string& section, const std::string& key,
                 const std::string& value);

// One value as the file has it; false if the key is not there. For a file with mistakes too.
bool find_setting(const std::string& text, const std::string& section, const std::string& key,
                  std::string& value);

// The WiFi password never goes over the network: Mainsail gets the file with it as
// kMaskedPassword, and a save that still has kMaskedPassword keeps the password in `current`
// (the file on the card). Typing a new one over it changes it.
extern const char* const kMaskedPassword;
std::string mask_password(const std::string& text);
std::string unmask_password(const std::string& saved, const std::string& current);

}  // namespace halyard
