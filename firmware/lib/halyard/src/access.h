// SPDX-License-Identifier: GPL-3.0-or-later
// Who may use Halyard, in Moonraker's model (its [authorization] section): clients whose
// address is in a trusted range need nothing, and everyone else needs the API key in an
// X-Api-Key header. Mainsail has no login, so a browser must be trusted. Slicers can send the
// key (OctoPrint's API key field). The default trusts the private ranges, as MainsailOS does:
// open on the home network, closed to anything routed in from outside.
//
// Addresses are 16 bytes, IPv4 as IPv4-mapped IPv6 (::ffff:a.b.c.d). Portable C++.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace halyard {

struct IpAddr {
  uint8_t b[16] = {};
  static IpAddr v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
  bool is_v4() const;  // IPv4-mapped
};

// "192.168.1.20", "::1" or "fe80::1%2" (the zone is ignored). False if it is not an address.
bool parse_ip(const std::string& text, IpAddr& out);
std::string ip_text(const IpAddr& a);

class Access {
 public:
  Access();

  // Comma- or space-separated ranges: "192.168.1.0/24, 10.0.0.5, fe80::/10". A bare address
  // is a range of one. False, and nothing changed, if any of them does not parse.
  bool set_trusted(const std::string& list, std::string& err);
  void trust_defaults();
  std::string trusted() const;  // as set_trusted takes it, normalised
  static const char* kDefaultTrusted;

  void set_api_key(const std::string& key) { api_key_ = key; }
  const std::string& api_key() const { return api_key_; }
  // 32 hex characters, as Moonraker's keys are, from a source of random 32-bit words.
  template <typename Rand>
  static std::string new_api_key(Rand rnd) {
    static const char hex[] = "0123456789abcdef";
    std::string k;
    while (k.size() < 32) {
      uint32_t r = rnd();
      for (int i = 0; i < 8; i++, r >>= 4) k += hex[r & 15];
    }
    return k;
  }

  bool trusts(const IpAddr& peer) const;
  // A request from `peer` carrying `key` (the X-Api-Key header, empty if none).
  bool allowed(const IpAddr& peer, const std::string& key) const;

 private:
  struct Range {
    IpAddr net;
    int bits;  // prefix length over the 16 bytes (an IPv4 /24 is 96 + 24)
  };
  static bool parse_range(const std::string& text, Range& out);
  std::vector<Range> trusted_;
  std::string api_key_;
};

}  // namespace halyard
