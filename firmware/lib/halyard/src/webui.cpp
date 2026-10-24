// SPDX-License-Identifier: GPL-3.0-or-later
#include "webui.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace halyard {

namespace {

bool is_file(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void rm_tree(const std::string& path, const std::function<void()>& busy = nullptr) {
  if (DIR* d = opendir(path.c_str())) {
    while (struct dirent* de = readdir(d)) {
      std::string n = de->d_name;
      if (n != "." && n != "..") rm_tree(path + "/" + n, busy);
    }
    closedir(d);
    rmdir(path.c_str());
  } else {
    unlink(path.c_str());
  }
  if (busy) busy();
}

// "a/b/c": every segment a plain name, no "..", nothing absolute. A leading "./" is dropped.
bool clean_path(std::string& p) {
  while (p.compare(0, 2, "./") == 0) p.erase(0, 2);
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty() || p[0] == '/') return false;
  size_t i = 0;
  while (i <= p.size()) {
    size_t end = p.find('/', i);
    if (end == std::string::npos) end = p.size();
    std::string seg = p.substr(i, end - i);
    if (seg.empty() || seg == "." || seg == "..") return false;
    i = end + 1;
  }
  return true;
}

void mkdirs(const std::string& root, const std::string& rel) {
  size_t i = 0;
  while ((i = rel.find('/', i)) != std::string::npos) {
    mkdir((root + "/" + rel.substr(0, i)).c_str(), 0755);
    i++;
  }
}

uint64_t octal(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n && p[i]; i++) {
    if (p[i] == ' ') continue;
    if (p[i] < '0' || p[i] > '7') break;
    v = v * 8 + (p[i] - '0');
  }
  return v;
}

std::string field(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len]) len++;
  return std::string((const char*)p, len);
}

const char* type_for(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (auto& c : ext) c = (char)tolower((unsigned char)c);
  static const struct {
    const char* ext;
    const char* type;
  } types[] = {
      {"html", "text/html"},       {"js", "text/javascript"},
      {"mjs", "text/javascript"},  {"css", "text/css"},
      {"json", "application/json"}, {"webmanifest", "application/manifest+json"},
      {"svg", "image/svg+xml"},    {"png", "image/png"},
      {"jpg", "image/jpeg"},       {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},        {"webp", "image/webp"},
      {"ico", "image/x-icon"},     {"woff", "font/woff"},
      {"woff2", "font/woff2"},     {"ttf", "font/ttf"},
      {"txt", "text/plain"},       {"wasm", "application/wasm"},
      {"map", "application/json"},
  };
  for (auto& t : types)
    if (ext == t.ext) return t.type;
  return "application/octet-stream";
}

}  // namespace

bool WebUi::installed() const {
  return is_file(root_ + "/index.html.gz") || is_file(root_ + "/index.html");
}

bool WebUi::lookup(const std::string& url_path, File& out) const {
  std::string p = url_path;
  while (!p.empty() && p[0] == '/') p.erase(0, 1);
  if (p.empty()) p = "index.html";
  if (!clean_path(p)) return false;
  for (int attempt = 0; attempt < 2; attempt++) {
    std::string full = root_ + "/" + p;
    if (is_file(full + ".gz")) {
      out.path = full + ".gz";
      out.gzip = true;
    } else if (is_file(full)) {
      out.path = full;
      out.gzip = false;
    } else {
      // Mainsail's own routes ("/files", "/settings/general") are not files.
      size_t slash = p.rfind('/');
      std::string last = slash == std::string::npos ? p : p.substr(slash + 1);
      if (attempt == 0 && last.find('.') == std::string::npos) {
        p = "index.html";
        continue;
      }
      return false;
    }
    out.content_type = type_for(p);
    out.immutable = p.compare(0, 7, "assets/") == 0;  // Vite puts a hash in these names
    return true;
  }
  return false;
}

// ---- installing ---------------------------------------------------------------------------

std::unique_ptr<WebUi::Install> WebUi::begin_install() {
  return std::unique_ptr<Install>(new Install(*this));
}

WebUi::Install::Install(WebUi& ui) : ui_(ui), staging_(ui.root_ + ".new") {
  rm_tree(staging_, ui_.on_busy);
  // A swap cut off after the UI was moved aside: that is the UI to keep.
  std::string old = ui_.root_ + ".old";
  if (!is_dir(ui_.root_) && is_dir(old)) rename(old.c_str(), ui_.root_.c_str());
  rm_tree(old, ui_.on_busy);
  if (mkdir(staging_.c_str(), 0755) != 0) fail("cannot create " + staging_);
}

WebUi::Install::~Install() {
  if (out_) fclose(out_);
  if (!finished_) rm_tree(staging_, ui_.on_busy);
}

// errno's reason: "No space left on device" when the card is full, "I/O error" for a card
// error, and so on.
static std::string write_error() {
  int e = errno;
  return std::string("write to Halyard's card failed: ") + (e ? strerror(e) : "no reason given");
}

bool WebUi::Install::fail(const std::string& msg) {
  if (!failed_) error_ = msg;
  failed_ = true;
  return false;
}

// One 512-byte header block. Regular files and directories are unpacked; anything else
// (links, pax headers) is skipped over.
bool WebUi::Install::header() {
  bool zero = true;
  for (int i = 0; i < 512 && zero; i++) zero = block_[i] == 0;
  if (zero) {
    if (++zero_blocks_ == 2) done_ = true;
    return true;
  }
  if (zero_blocks_) return fail("data after the end of the archive");
  uint32_t sum = 0;
  for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : block_[i];
  if (sum != octal(block_ + 148, 8)) return fail("not a tar archive (header checksum)");
  std::string name = field(block_, 100);
  if (memcmp(block_ + 257, "ustar", 5) == 0) {
    std::string prefix = field(block_ + 345, 155);
    if (!prefix.empty()) name = prefix + "/" + name;
  }
  char type = (char)block_[156];
  left_ = octal(block_ + 124, 12);
  if (type == '5') {
    if (name == "." || name == "./") return true;  // the archive's root
    if (!clean_path(name)) return fail("bad path: " + name);
    mkdirs(staging_, name + "/");
    return true;
  }
  if (type != '0' && type != '\0') return true;  // its data, if any, is skipped
  if (!clean_path(name)) return fail("bad path: " + name);
  mkdirs(staging_, name);
  out_path_ = staging_ + "/" + name;
  out_ = fopen(out_path_.c_str(), "wb");
  if (!out_) return fail("cannot write " + name);
  files_++;
  if (ui_.on_busy) ui_.on_busy();
  if (left_ == 0) {
    fclose(out_);
    out_ = nullptr;
  }
  return true;
}

bool WebUi::Install::feed(const uint8_t* data, size_t len) {
  if (failed_) return false;
  while (len > 0) {
    size_t n = std::min(len, sizeof block_ - have_);
    memcpy(block_ + have_, data, n);
    have_ += n;
    data += n;
    len -= n;
    if (have_ < sizeof block_) break;
    have_ = 0;
    if (done_) continue;  // tar pads the end with more zero blocks
    if (left_ == 0) {
      if (!header()) return false;
      continue;
    }
    size_t take = (size_t)std::min<uint64_t>(left_, sizeof block_);
    left_ -= take;
    if (out_) {
      errno = 0;
      if (fwrite(block_, 1, take, out_) != take) return fail(write_error());
      bytes_ += take;
      if (left_ == 0) {
        errno = 0;
        bool ok = fclose(out_) == 0;
        out_ = nullptr;
        if (!ok) return fail(write_error());
      }
    }
  }
  return true;
}

bool WebUi::Install::finish() {
  if (failed_) return false;
  if (!done_ || left_ || have_) return fail("the archive is incomplete");
  if (!is_file(staging_ + "/index.html.gz") && !is_file(staging_ + "/index.html"))
    return fail("no index.html in the archive");
  std::string old = ui_.root_ + ".old", prev = ui_.root_ + ".prev";
  bool had = is_dir(ui_.root_);
  if (had) rm_tree(prev, ui_.on_busy);  // the one before the one being replaced
  if (had && rename(ui_.root_.c_str(), old.c_str()) != 0) return fail("cannot replace the old UI");
  if (rename(staging_.c_str(), ui_.root_.c_str()) != 0) {
    if (had) rename(old.c_str(), ui_.root_.c_str());
    return fail("cannot move the new UI into place");
  }
  finished_ = true;
  if (had) rename(old.c_str(), prev.c_str());
  return true;
}

namespace {
std::string version_in(const std::string& dir) {
  FILE* f = fopen((dir + "/.version").c_str(), "rb");
  if (!f) return "";
  char buf[40];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  std::string v(buf, n);
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
  for (char c : v)
    if (c < 0x20 || c > 0x7e) return "";
  return v;
}
}  // namespace

std::string WebUi::version() const { return version_in(root_); }

std::string WebUi::previous_version() const {
  std::string prev = root_ + ".prev";
  if (!is_file(prev + "/index.html.gz") && !is_file(prev + "/index.html")) return "";
  std::string v = version_in(prev);
  return v.empty() ? "unknown" : v;
}

bool WebUi::restore_previous(std::string& err) {
  std::string prev = root_ + ".prev", old = root_ + ".old";
  if (previous_version().empty()) return err = "there is no previous Mainsail", false;
  rm_tree(old, on_busy);
  if (rename(root_.c_str(), old.c_str()) != 0) return err = "cannot move the current UI aside", false;
  if (rename(prev.c_str(), root_.c_str()) != 0) {
    rename(old.c_str(), root_.c_str());
    return err = "cannot move the previous UI into place", false;
  }
  rename(old.c_str(), prev.c_str());
  return true;
}

}  // namespace halyard
