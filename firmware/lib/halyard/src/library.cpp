// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include "gcode_line.h"

namespace halyard {

namespace {

constexpr size_t kMaxLine = 4096;     // longer lines are cut; no G-code command comes close
constexpr size_t kMaxComment = 256;   // kept of a comment when reading lines for an upload

std::string stem(const std::string& name) {
  size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool ends_with(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

uint32_t file_size(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? (uint32_t)st.st_size : 0;
}

}  // namespace

bool read_file(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[256];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

// Writes via a temporary file, so a reset mid-write leaves the old contents or the new.
// FAT's rename does not replace, hence the remove.
bool write_file(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  if (ok) {
    ::remove(path.c_str());
    ok = rename(tmp.c_str(), path.c_str()) == 0;
  }
  if (!ok) ::remove(tmp.c_str());
  return ok;
}

// ---- line source ------------------------------------------------------------------------

FileLineSource::~FileLineSource() {
  if (f_) fclose(f_);
}

bool FileLineSource::next(std::string& line) {
  line.clear();
  if (!f_) return false;
  bool any = false;
  size_t cap = kMaxLine;
  int c;
  while ((c = getc(f_)) != EOF) {
    read_++;
    any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (c == ';' && cap == kMaxLine) cap = std::min(kMaxLine, line.size() + kMaxComment);
    if (line.size() < cap) line += (char)c;
  }
  return any;
}

// ---- writer -----------------------------------------------------------------------------

Library::Writer::Writer(Library& lib, std::string name, FILE* f)
    : lib_(lib), name_(std::move(name)), f_(f),
      parser_([this](int w, int h, std::string& rel) {
        FILE* t = lib_.open_thumb(name_, w, h, rel);
        if (t) thumbs_opened_.push_back(rel);
        return t;
      }) {}

Library::Writer::~Writer() {
  if (!done_) abort();
}

void Library::Writer::feed_line() {
  if (!line_.empty() && line_.back() == '\r') line_.pop_back();
  parser_.line(line_);
  line_.clear();
}

bool Library::Writer::write(const uint8_t* data, size_t len) {
  if (failed_ || done_) return false;
  errno = 0;
  if (fwrite(data, 1, len, f_) != len) {
    int e = errno;
    failed_ = true;
    error_ = "write to Halyard's card failed at byte " + std::to_string(size_) + ": " +
             (e ? strerror(e) : "no reason given");
    return false;
  }
  size_ += len;
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n') feed_line();
    else if (line_.size() < kMaxLine) line_ += c;
  }
  return true;
}

bool Library::Writer::finish(std::string& err) {
  if (done_) return false;
  if (!line_.empty()) feed_line();
  bool ok = !failed_;
  errno = 0;
  if (fclose(f_) != 0 && ok) {
    int e = errno;
    ok = false;
    error_ = std::string("closing the file on Halyard's card failed: ") +
             (e ? strerror(e) : "no reason given");
  }
  f_ = nullptr;
  if (!ok) {
    err = error_;
    abort();
    return false;
  }
  done_ = true;
  Metadata meta = parser_.finish();

  // Replace an older file of the same name, keeping its printer name so the next hand-off
  // overwrites its own copy there; that copy is stale now.
  LibraryEntry e;
  bool had = lib_.load(name_, e);
  if (had) {
    std::vector<std::string> keep;
    for (auto& t : meta.thumbnails) keep.push_back(t.relative_path);
    for (auto& t : e.meta.thumbnails)
      if (std::find(keep.begin(), keep.end(), t.relative_path) == keep.end())
        ::remove((lib_.root_ + "/" + t.relative_path).c_str());
  }
  for (auto& rel : thumbs_opened_) {
    bool listed = false;
    for (auto& t : meta.thumbnails) listed |= t.relative_path == rel;
    if (!listed) ::remove((lib_.root_ + "/" + rel).c_str());
  }
  std::string final_path = lib_.path(name_);
  ::remove(final_path.c_str());
  if (::rename((final_path + ".part").c_str(), final_path.c_str()) != 0) {
    err = "rename failed";
    ::remove((final_path + ".part").c_str());
    return false;
  }
  e.name = name_;
  e.size = size_;
  e.meta = meta;
  e.printer_size = 0;
  if (!lib_.save(e)) {
    err = "stored, but its metadata could not be written";
    return false;
  }
  return true;
}

void Library::Writer::abort() {
  if (done_) return;
  done_ = true;
  if (f_) fclose(f_);
  f_ = nullptr;
  parser_.finish();  // closes a thumbnail left open
  ::remove((lib_.path(name_) + ".part").c_str());
  for (auto& rel : thumbs_opened_) ::remove((lib_.root_ + "/" + rel).c_str());
}

// ---- library ----------------------------------------------------------------------------

bool Library::begin() {
  mkdir(root_.c_str(), 0755);
  mkdir((root_ + "/.thumbs").c_str(), 0755);
  mkdir((root_ + "/.meta").c_str(), 0755);
  struct stat st;
  return stat((root_ + "/.meta").c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Library::valid_name(const std::string& name) {
  if (name.empty() || name.size() > 120 || name[0] == '.') return false;
  if (ends_with(name, ".part") || ends_with(name, ".tmp")) return false;
  for (unsigned char c : name)
    if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|')
      return false;
  return true;
}

std::unique_ptr<Library::Writer> Library::create(const std::string& name, std::string& err) {
  if (!valid_name(name)) {
    err = "invalid file name";
    return nullptr;
  }
  FILE* f = fopen((path(name) + ".part").c_str(), "wb");
  if (!f) {
    err = "cannot create file on Halyard's card";
    return nullptr;
  }
  return std::unique_ptr<Writer>(new Writer(*this, name, f));
}

FILE* Library::open_thumb(const std::string& name, int w, int h, std::string& rel) {
  rel = ".thumbs/" + stem(name) + "-" + std::to_string(w) + "x" + std::to_string(h) + ".png";
  return fopen((root_ + "/" + rel).c_str(), "wb");
}

std::vector<std::string> Library::names() const {
  std::vector<std::string> out;
  DIR* d = opendir(root_.c_str());
  if (!d) return out;
  while (struct dirent* de = readdir(d)) {
    std::string n = de->d_name;
    if (!valid_name(n)) continue;  // dot entries, .part files
    struct stat st;
    if (stat(path(n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    out.push_back(n);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool Library::load(const std::string& name, LibraryEntry& e) const {
  std::string text;
  if (!read_file(meta_path(name), text)) return false;
  e = LibraryEntry();
  e.name = name;
  e.meta = Metadata::parse(text);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string l = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (starts_with(l, "size=")) e.size = strtoul(l.c_str() + 5, nullptr, 10);
    else if (starts_with(l, "printer_name=")) e.printer_name = l.substr(13);
    else if (starts_with(l, "printer_size=")) e.printer_size = strtoul(l.c_str() + 13, nullptr, 10);
  }
  return true;
}

bool Library::save(const LibraryEntry& e) {
  std::string s = e.meta.serialize() + "size=" + std::to_string(e.size) + "\n";
  if (!e.printer_name.empty()) {
    s += "printer_name=" + e.printer_name + "\n";
    s += "printer_size=" + std::to_string(e.printer_size) + "\n";
  }
  return write_file(meta_path(e.name), s);
}

bool Library::get(const std::string& name, LibraryEntry& e) {
  if (!valid_name(name)) return false;
  uint32_t size = file_size(path(name));
  FILE* f = fopen(path(name).c_str(), "rb");
  if (!f) return false;
  if (load(name, e) && e.size == size) {
    fclose(f);
    return true;
  }
  // No metadata, or the file changed behind Halyard's back: scan it. A changed file's copy
  // on the printer is stale, but its printer name is kept for the next hand-off.
  std::vector<std::string> opened;
  MetadataParser p([&](int w, int h, std::string& rel) {
    FILE* t = open_thumb(name, w, h, rel);
    if (t) opened.push_back(rel);
    return t;
  });
  std::string line;
  int c;
  while ((c = getc(f)) != EOF) {
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      p.line(line);
      line.clear();
    } else if (line.size() < kMaxLine) {
      line += (char)c;
    }
  }
  if (!line.empty()) p.line(line);
  fclose(f);
  std::string printer_name = e.printer_name;
  e = LibraryEntry();
  e.name = name;
  e.size = size;
  e.meta = p.finish();
  e.printer_name = printer_name;
  for (auto& rel : opened) {
    bool listed = false;
    for (auto& t : e.meta.thumbnails) listed |= t.relative_path == rel;
    if (!listed) ::remove((root_ + "/" + rel).c_str());
  }
  save(e);
  return true;
}

void Library::remove_thumbs(const Metadata& m) {
  for (auto& t : m.thumbnails) ::remove((root_ + "/" + t.relative_path).c_str());
}

bool Library::remove(const std::string& name) {
  if (!valid_name(name)) return false;
  LibraryEntry e;
  if (load(name, e)) remove_thumbs(e.meta);
  ::remove(meta_path(name).c_str());
  return ::remove(path(name).c_str()) == 0;
}

bool Library::rename(const std::string& from, const std::string& to, std::string& err) {
  if (!valid_name(from) || !valid_name(to)) return err = "not a valid file name", false;
  LibraryEntry e;
  if (!get(from, e)) return err = "no such file", false;
  struct stat st;
  if (stat(path(to).c_str(), &st) == 0) return err = to + " already exists", false;
  if (::rename(path(from).c_str(), path(to).c_str()) != 0) return err = "rename failed", false;
  // Thumbnails are named after the file; a later upload under the old name would reuse
  // (and overwrite) them otherwise.
  for (auto& t : e.meta.thumbnails) {
    std::string rel = ".thumbs/" + stem(to) + "-" + std::to_string(t.width) + "x" +
                      std::to_string(t.height) + ".png";
    if (::rename((root_ + "/" + t.relative_path).c_str(), (root_ + "/" + rel).c_str()) == 0)
      t.relative_path = rel;
  }
  ::remove(meta_path(from).c_str());
  e.name = to;
  save(e);
  return true;
}

bool Library::set_printer_file(const std::string& name, const std::string& printer_name,
                               uint32_t printer_size) {
  LibraryEntry e;
  if (!get(name, e)) return false;
  e.printer_name = printer_name;
  e.printer_size = printer_size;
  return save(e);
}

std::unique_ptr<FileLineSource> Library::open_lines(const std::string& name) const {
  if (!valid_name(name)) return nullptr;
  FILE* f = fopen(path(name).c_str(), "rb");
  if (!f) return nullptr;
  return std::unique_ptr<FileLineSource>(new FileLineSource(f, file_size(path(name))));
}

std::vector<std::string> Library::printer_names(const std::string& except) const {
  std::vector<std::string> out;
  for (auto& n : names()) {
    LibraryEntry e;
    if (n != except && load(n, e) && !e.printer_name.empty()) out.push_back(e.printer_name);
  }
  return out;
}

// ---- 8.3 names --------------------------------------------------------------------------

std::string short_name(const std::string& long_name, const std::vector<std::string>& taken) {
  std::string base;
  for (char c : stem(long_name)) {
    if (isalnum((unsigned char)c)) base += (char)toupper((unsigned char)c);
    if (base.size() == 6) break;
  }
  if (base.empty()) base = "JOB";
  for (int n = 1; n < 100000; n++) {
    std::string suffix = "~" + std::to_string(n);
    std::string cand = base.substr(0, std::min(base.size(), 8 - suffix.size())) + suffix + ".GCO";
    if (std::find(taken.begin(), taken.end(), cand) == taken.end()) return cand;
  }
  return std::string();  // 99999 names taken; not a real card
}

}  // namespace halyard
