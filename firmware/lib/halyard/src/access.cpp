// SPDX-License-Identifier: GPL-3.0-or-later
#include "access.h"

#include <cstdio>
#include <cstdlib>

namespace halyard {

namespace {

bool parse_v4(const std::string& s, uint8_t out[4]) {
  int part = 0;
  size_t i = 0;
  while (part < 4) {
    if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
    int v = 0, digits = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
      v = v * 10 + (s[i++] - '0');
      if (++digits > 3 || v > 255) return false;
    }
    out[part++] = (uint8_t)v;
    if (part < 4) {
      if (i >= s.size() || s[i] != '.') return false;
      i++;
    }
  }
  return i == s.size();
}

// Groups of up to four hex digits, one "::" at most, an IPv4 address allowed at the end.
bool parse_v6(const std::string& s, uint8_t out[16]) {
  uint16_t head[8], tail[8];
  int nh = 0, nt = 0;
  bool gap = false;
  size_t i = 0;
  if (s.compare(0, 2, "::") == 0) {
    gap = true;
    i = 2;
  }
  uint8_t v4[4];
  bool has_v4 = false;
  while (i < s.size()) {
    size_t end = s.find(':', i);
    std::string g = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
    if (end == std::string::npos && g.find('.') != std::string::npos) {
      if (!parse_v4(g, v4)) return false;
      has_v4 = true;
      i = s.size();
      break;
    }
    if (g.empty() || g.size() > 4) return false;
    uint16_t v = 0;
    for (char c : g) {
      if (!isxdigit((unsigned char)c)) return false;
      v = (uint16_t)(v * 16 + (isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10)));
    }
    uint16_t* dst = gap ? tail : head;
    int& n = gap ? nt : nh;
    if (n >= 8) return false;
    dst[n++] = v;
    if (end == std::string::npos) {
      i = s.size();
    } else if (s.compare(end, 2, "::") == 0) {
      if (gap) return false;
      gap = true;
      i = end + 2;
    } else {
      i = end + 1;
      if (i == s.size()) return false;  // a trailing single colon
    }
  }
  int groups = nh + nt + (has_v4 ? 2 : 0);
  if (gap ? groups > 7 : groups != 8) return false;
  uint16_t all[8] = {};
  for (int k = 0; k < nh; k++) all[k] = head[k];
  int t0 = 8 - nt - (has_v4 ? 2 : 0);
  for (int k = 0; k < nt; k++) all[t0 + k] = tail[k];
  for (int k = 0; k < 8; k++) {
    out[2 * k] = (uint8_t)(all[k] >> 8);
    out[2 * k + 1] = (uint8_t)all[k];
  }
  if (has_v4)
    for (int k = 0; k < 4; k++) out[12 + k] = v4[k];
  return true;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

const char* Access::kDefaultTrusted =
    "10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, fe80::/10, ::1/128";

IpAddr IpAddr::v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  IpAddr r;
  r.b[10] = r.b[11] = 0xff;
  r.b[12] = a;
  r.b[13] = b;
  r.b[14] = c;
  r.b[15] = d;
  return r;
}

bool IpAddr::is_v4() const {
  for (int i = 0; i < 10; i++)
    if (b[i]) return false;
  return b[10] == 0xff && b[11] == 0xff;
}

bool parse_ip(const std::string& text, IpAddr& out) {
  std::string s = text.substr(0, text.find('%'));
  IpAddr a;
  uint8_t v4[4];
  if (parse_v4(s, v4)) {
    out = IpAddr::v4(v4[0], v4[1], v4[2], v4[3]);
    return true;
  }
  if (s.find(':') == std::string::npos || !parse_v6(s, a.b)) return false;
  out = a;
  return true;
}

std::string ip_text(const IpAddr& a) {
  char buf[48];
  if (a.is_v4()) {
    snprintf(buf, sizeof buf, "%d.%d.%d.%d", a.b[12], a.b[13], a.b[14], a.b[15]);
    return buf;
  }
  // Plain groups; the longest run of zero groups (two or more) becomes "::".
  uint16_t g[8];
  for (int k = 0; k < 8; k++) g[k] = (uint16_t)(a.b[2 * k] << 8 | a.b[2 * k + 1]);
  int best = -1, best_len = 1;
  for (int k = 0; k < 8;) {
    int n = 0;
    while (k + n < 8 && g[k + n] == 0) n++;
    if (n > best_len) best = k, best_len = n;
    k += n ? n : 1;
  }
  std::string out;
  for (int k = 0; k < 8; k++) {
    if (k == best) {
      out += "::";
      k += best_len - 1;
      continue;
    }
    if (!out.empty() && out.back() != ':') out += ':';
    snprintf(buf, sizeof buf, "%x", g[k]);
    out += buf;
  }
  return out;
}

Access::Access() { trust_defaults(); }

bool Access::parse_range(const std::string& text, Range& out) {
  size_t slash = text.find('/');
  if (!parse_ip(text.substr(0, slash), out.net)) return false;
  bool v4 = out.net.is_v4();
  int max = v4 ? 32 : 128;
  int bits = max;
  if (slash != std::string::npos) {
    std::string n = text.substr(slash + 1);
    if (n.empty() || n.size() > 3 || n.find_first_not_of("0123456789") != std::string::npos)
      return false;
    bits = atoi(n.c_str());
    if (bits > max) return false;
  }
  out.bits = v4 ? 96 + bits : bits;
  for (int i = 0; i < 16; i++) {  // clear the host part, so trusted() prints the network
    int keep = out.bits - 8 * i;
    if (keep <= 0) out.net.b[i] = 0;
    else if (keep < 8) out.net.b[i] &= (uint8_t)(0xff << (8 - keep));
  }
  return true;
}

bool Access::set_trusted(const std::string& list, std::string& err) {
  std::vector<Range> ranges;
  size_t i = 0;
  while (i <= list.size()) {
    size_t end = list.find_first_of(", ", i);
    if (end == std::string::npos) end = list.size();
    std::string item = trim(list.substr(i, end - i));
    i = end + 1;
    if (item.empty()) continue;
    Range r;
    if (!parse_range(item, r)) {
      err = "not an address or range: " + item;
      return false;
    }
    ranges.push_back(r);
  }
  trusted_ = ranges;
  return true;
}

void Access::trust_defaults() {
  std::string err;
  set_trusted(kDefaultTrusted, err);
}

std::string Access::trusted() const {
  std::string out;
  for (auto& r : trusted_) {
    if (!out.empty()) out += ", ";
    bool v4 = r.net.is_v4() && r.bits >= 96;
    out += ip_text(r.net) + "/" + std::to_string(v4 ? r.bits - 96 : r.bits);
  }
  return out;
}

bool Access::trusts(const IpAddr& peer) const {
  for (auto& r : trusted_) {
    int bits = r.bits, i = 0;
    for (; bits >= 8; bits -= 8, i++)
      if (peer.b[i] != r.net.b[i]) break;
    if (bits >= 8) continue;
    if (bits == 0 || ((peer.b[i] ^ r.net.b[i]) & (uint8_t)(0xff << (8 - bits))) == 0) return true;
  }
  return false;
}

bool Access::allowed(const IpAddr& peer, const std::string& key) const {
  if (trusts(peer)) return true;
  if (api_key_.empty() || key.size() != api_key_.size()) return false;
  uint8_t diff = 0;  // the same time for every wrong key of the right length
  for (size_t i = 0; i < key.size(); i++) diff |= (uint8_t)(key[i] ^ api_key_[i]);
  return diff == 0;
}

}  // namespace halyard
