// SPDX-License-Identifier: GPL-3.0-or-later
// Who may use Halyard (Access, Moonraker's trusted ranges and API key) and the
// web UI on the card (WebUi: a tar install that replaces the old UI only when complete, and
// how request paths map to the gzipped files). Run: pio test -e native
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "access.h"
#include "webui.h"
#include "zip_tar.h"

using namespace halyard;

static std::string g_root;

static void rm_tree(const std::string& path) {
  if (DIR* d = opendir(path.c_str())) {
    while (struct dirent* de = readdir(d)) {
      std::string n = de->d_name;
      if (n != "." && n != "..") rm_tree(path + "/" + n);
    }
    closedir(d);
    rmdir(path.c_str());
  } else {
    unlink(path.c_str());
  }
}

static bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string slurp(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
  }
  return s;
}

void setUp() {
  char tmpl[] = "/tmp/halyard-web-XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(tmpl));
  g_root = tmpl;
}

void tearDown() { rm_tree(g_root); }

static IpAddr ip(const char* text) {
  IpAddr a;
  TEST_ASSERT_TRUE_MESSAGE(parse_ip(text, a), text);
  return a;
}

// ---- addresses ------------------------------------------------------------------------------

void test_parse_addresses() {
  const char* good[][2] = {
      {"192.168.1.20", "192.168.1.20"}, {"0.0.0.0", "0.0.0.0"},
      {"::1", "::1"},                   {"::", "::"},
      {"fe80::1%2", "fe80::1"},         {"FE80::A:b", "fe80::a:b"},
      {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
      {"2001:db8::", "2001:db8::"},     {"1:0:0:2:0:0:0:3", "1:0:0:2::3"},
      {"::ffff:10.1.2.3", "10.1.2.3"},  // IPv4-mapped, as lwIP reports IPv4 peers
  };
  for (auto& g : good) TEST_ASSERT_EQUAL_STRING_MESSAGE(g[1], ip_text(ip(g[0])).c_str(), g[0]);
  TEST_ASSERT_TRUE(ip("::ffff:10.1.2.3").is_v4());
  TEST_ASSERT_FALSE(ip("::1").is_v4());
  const char* bad[] = {"",        "1.2.3",   "1.2.3.4.5", "256.1.1.1", "1.2.3.04x", "1:2",
                       "1:::2",   "1::2::3", "12345::",   "1:2:3:4:5:6:7:8:9", ":1:2",
                       "1:2:3:",  "g::1",    "1.2.3.4/8", "host.local"};
  IpAddr a;
  for (auto b : bad) TEST_ASSERT_FALSE_MESSAGE(parse_ip(b, a), b);
}

// ---- access ---------------------------------------------------------------------------------

void test_default_trusts_private_ranges() {
  Access acc;
  for (auto t : {"192.168.1.20", "10.0.0.1", "10.255.255.255", "172.16.0.1", "172.31.255.254",
                 "127.0.0.1", "169.254.3.4", "fe80::1234", "::1", "::ffff:192.168.0.9"})
    TEST_ASSERT_TRUE_MESSAGE(acc.trusts(ip(t)), t);
  for (auto t : {"8.8.8.8", "172.32.0.1", "172.15.255.255", "192.169.0.1", "11.0.0.1",
                 "2001:db8::1", "fe00::1", "::2"})
    TEST_ASSERT_FALSE_MESSAGE(acc.trusts(ip(t)), t);
  TEST_ASSERT_EQUAL_STRING(Access::kDefaultTrusted, acc.trusted().c_str());
}

void test_api_key_admits_the_untrusted() {
  Access acc;
  IpAddr outside = ip("203.0.113.7"), inside = ip("192.168.1.5");
  TEST_ASSERT_FALSE(acc.allowed(outside, ""));  // no key set: nothing gets in from outside
  TEST_ASSERT_FALSE(acc.allowed(outside, "anything"));
  uint32_t n = 0;
  std::string key = Access::new_api_key([&n] { return 0x9e3779b9u * ++n; });
  TEST_ASSERT_EQUAL(32, key.size());
  TEST_ASSERT_EQUAL(std::string::npos, key.find_first_not_of("0123456789abcdef"));
  acc.set_api_key(key);
  TEST_ASSERT_TRUE(acc.allowed(outside, key));
  TEST_ASSERT_FALSE(acc.allowed(outside, ""));
  TEST_ASSERT_FALSE(acc.allowed(outside, key.substr(1)));
  std::string wrong = key;
  wrong[31] = wrong[31] == '0' ? '1' : '0';
  TEST_ASSERT_FALSE(acc.allowed(outside, wrong));
  TEST_ASSERT_TRUE(acc.allowed(inside, ""));  // trusted needs no key
  TEST_ASSERT_TRUE(acc.allowed(inside, wrong));
}

void test_set_trusted() {
  Access acc;
  std::string err;
  TEST_ASSERT_TRUE(acc.set_trusted("192.168.1.77/24,10.0.0.5  2001:db8::/32", err));
  TEST_ASSERT_EQUAL_STRING("192.168.1.0/24, 10.0.0.5/32, 2001:db8::/32", acc.trusted().c_str());
  TEST_ASSERT_TRUE(acc.trusts(ip("192.168.1.200")));
  TEST_ASSERT_FALSE(acc.trusts(ip("192.168.2.1")));
  TEST_ASSERT_TRUE(acc.trusts(ip("10.0.0.5")));
  TEST_ASSERT_FALSE(acc.trusts(ip("10.0.0.6")));
  TEST_ASSERT_TRUE(acc.trusts(ip("2001:db8:ffff::1")));
  TEST_ASSERT_FALSE(acc.trusts(ip("2001:db9::1")));
  // A bad entry changes nothing.
  TEST_ASSERT_FALSE(acc.set_trusted("192.168.1.0/24, 10.0.0.0/33", err));
  TEST_ASSERT_TRUE(err.find("10.0.0.0/33") != std::string::npos);
  TEST_ASSERT_FALSE(acc.set_trusted("fe80::/129", err));
  TEST_ASSERT_FALSE(acc.set_trusted("10.0.0.0/", err));
  TEST_ASSERT_FALSE(acc.set_trusted("halyard.local", err));
  TEST_ASSERT_EQUAL_STRING("192.168.1.0/24, 10.0.0.5/32, 2001:db8::/32", acc.trusted().c_str());
  // Odd prefix lengths, everything, and nothing.
  TEST_ASSERT_TRUE(acc.set_trusted("172.16.0.0/12", err));
  TEST_ASSERT_TRUE(acc.trusts(ip("172.20.1.1")));
  TEST_ASSERT_FALSE(acc.trusts(ip("172.32.0.0")));
  TEST_ASSERT_TRUE(acc.set_trusted("0.0.0.0/0", err));
  TEST_ASSERT_TRUE(acc.trusts(ip("8.8.8.8")));
  TEST_ASSERT_FALSE(acc.trusts(ip("::1")));  // an IPv4 range covers IPv4 only
  TEST_ASSERT_TRUE(acc.set_trusted("", err));
  TEST_ASSERT_EQUAL_STRING("", acc.trusted().c_str());
  TEST_ASSERT_FALSE(acc.trusts(ip("127.0.0.1")));
  acc.trust_defaults();
  TEST_ASSERT_TRUE(acc.trusts(ip("127.0.0.1")));
}

// ---- the web UI -----------------------------------------------------------------------------

// A ustar archive, built by hand.
struct Tar {
  std::string data;
  void entry(const std::string& name, const std::string& body, char type = '0') {
    char h[512] = {};
    snprintf(h, 100, "%s", name.c_str());
    snprintf(h + 100, 8, "%07o", type == '5' ? 0755 : 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011o", (unsigned)body.size());
    snprintf(h + 136, 12, "%011o", 0);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (uint8_t)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    data.append(h, 512);
    data += body;
    data.append((512 - body.size() % 512) % 512, '\0');
  }
  std::string done() { return data + std::string(1024, '\0'); }
};

static std::string mainsail_tar(const std::string& version) {
  Tar t;
  t.entry("./", "", '5');
  t.entry("./index.html.gz", "GZ<html " + version + ">");
  t.entry("./.version", version + "\n");
  t.entry("./assets/", "", '5');
  t.entry("./assets/index-AbC123.js.gz", std::string(1500, 'j'));  // spans three blocks
  t.entry("./fonts/roboto.woff2", "WOFF2");  // no directory entry for fonts/ first
  t.entry("./config.json", "{}");
  t.entry("./empty.txt", "");
  t.entry("./link", "", '2');  // a symlink: skipped
  return t.done();
}

static bool install(WebUi& ui, const std::string& tar, size_t chunk, std::string* err = nullptr) {
  auto in = ui.begin_install();
  bool ok = true;
  for (size_t pos = 0; ok && pos < tar.size(); pos += chunk)
    ok = in->feed((const uint8_t*)tar.data() + pos, std::min(chunk, tar.size() - pos));
  ok = ok && in->finish();
  if (err) *err = in->error();
  return ok;
}

void test_webui_install_and_lookup() {
  std::string root = g_root + "/www";
  WebUi ui(root);
  TEST_ASSERT_FALSE(ui.installed());
  WebUi::File f;
  TEST_ASSERT_FALSE(ui.lookup("/", f));
  for (size_t chunk : {1, 7, 512, 513, 100000}) {
    std::string err;
    TEST_ASSERT_TRUE_MESSAGE(install(ui, mainsail_tar("v1"), chunk, &err), err.c_str());
  }
  TEST_ASSERT_TRUE(ui.installed());
  TEST_ASSERT_FALSE(exists(root + ".new"));
  TEST_ASSERT_FALSE(exists(root + ".old"));
  TEST_ASSERT_FALSE(exists(root + "/link"));

  TEST_ASSERT_TRUE(ui.lookup("/", f));
  TEST_ASSERT_EQUAL_STRING((root + "/index.html.gz").c_str(), f.path.c_str());
  TEST_ASSERT_EQUAL_STRING("text/html", f.content_type.c_str());
  TEST_ASSERT_TRUE(f.gzip);
  TEST_ASSERT_FALSE(f.immutable);
  TEST_ASSERT_EQUAL_STRING("GZ<html v1>", slurp(f.path).c_str());

  TEST_ASSERT_TRUE(ui.lookup("/assets/index-AbC123.js", f));
  TEST_ASSERT_EQUAL_STRING("text/javascript", f.content_type.c_str());
  TEST_ASSERT_TRUE(f.gzip);
  TEST_ASSERT_TRUE(f.immutable);
  TEST_ASSERT_EQUAL(1500, slurp(f.path).size());

  TEST_ASSERT_TRUE(ui.lookup("/fonts/roboto.woff2", f));
  TEST_ASSERT_EQUAL_STRING("font/woff2", f.content_type.c_str());
  TEST_ASSERT_FALSE(f.gzip);
  TEST_ASSERT_EQUAL_STRING("WOFF2", slurp(f.path).c_str());

  TEST_ASSERT_TRUE(ui.lookup("/config.json", f));
  TEST_ASSERT_EQUAL_STRING("application/json", f.content_type.c_str());
  TEST_ASSERT_TRUE(ui.lookup("/empty.txt", f));
  TEST_ASSERT_EQUAL(0, slurp(f.path).size());

  // Mainsail's routes are its index page; missing files are not.
  TEST_ASSERT_TRUE(ui.lookup("/settings/general", f));
  TEST_ASSERT_EQUAL_STRING((root + "/index.html.gz").c_str(), f.path.c_str());
  TEST_ASSERT_FALSE(ui.lookup("/assets/missing.js", f));
  TEST_ASSERT_FALSE(ui.lookup("/../secret", f));
  TEST_ASSERT_FALSE(ui.lookup("/assets/../../x", f));
  TEST_ASSERT_TRUE(ui.lookup("//etc/passwd", f));  // a route like any other: stays inside
  TEST_ASSERT_EQUAL_STRING((root + "/index.html.gz").c_str(), f.path.c_str());
}

// Found on hardware: installing over an existing UI reset Halyard (task watchdog). Removing
// the old files is one long stretch of card work, and it must pause between them.
void test_webui_install_pauses_between_files() {
  std::string root = g_root + "/www";
  WebUi ui(root);
  Tar big;
  big.entry("index.html.gz", "GZ");
  for (int i = 0; i < 50; i++) big.entry("assets/f" + std::to_string(i) + ".js", "x");
  TEST_ASSERT_TRUE(install(ui, big.done(), 4096));
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v2"), 4096));  // the 51 files are kept as .prev
  int calls = 0;
  ui.on_busy = [&] { calls++; };
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v3"), 4096));  // and removed now
  TEST_ASSERT_GREATER_OR_EQUAL(51 + 5, calls);  // each old file removed, each new one written
  // A staging folder left by a reset mid-install is removed the same way.
  std::string full = big.done();
  { auto in = ui.begin_install(); in->feed((const uint8_t*)full.data(), full.size() - 1024); }
  calls = 0;
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v4"), 4096));
  TEST_ASSERT_GREATER_OR_EQUAL(5, calls);
}

void test_webui_keeps_the_previous_one() {
  std::string root = g_root + "/www";
  WebUi ui(root);
  TEST_ASSERT_EQUAL_STRING("", ui.version().c_str());
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v1"), 4096));
  TEST_ASSERT_EQUAL_STRING("v1", ui.version().c_str());
  TEST_ASSERT_EQUAL_STRING("", ui.previous_version().c_str());  // the first install: none
  std::string err;
  TEST_ASSERT_FALSE(ui.restore_previous(err));
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v2"), 4096));
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v3"), 4096));
  TEST_ASSERT_EQUAL_STRING("v3", ui.version().c_str());
  TEST_ASSERT_EQUAL_STRING("v2", ui.previous_version().c_str());  // only one is kept
  TEST_ASSERT_TRUE_MESSAGE(ui.restore_previous(err), err.c_str());
  TEST_ASSERT_EQUAL_STRING("v2", ui.version().c_str());
  TEST_ASSERT_EQUAL_STRING("v3", ui.previous_version().c_str());  // and back again
  WebUi::File f;
  TEST_ASSERT_TRUE(ui.lookup("/", f));
  TEST_ASSERT_EQUAL_STRING("GZ<html v2>", slurp(f.path).c_str());
  TEST_ASSERT_FALSE(exists(root + ".old"));
  // A reset between moving the UI aside and moving the new one in: the next install finds
  // no UI and a .old, and keeps that.
  TEST_ASSERT_EQUAL(0, rename(root.c_str(), (root + ".old").c_str()));
  { auto in = ui.begin_install(); }
  TEST_ASSERT_EQUAL_STRING("v2", ui.version().c_str());
}

void test_webui_failed_install_keeps_the_old_one() {
  std::string root = g_root + "/www";
  WebUi ui(root);
  TEST_ASSERT_TRUE(install(ui, mainsail_tar("v1"), 4096));
  WebUi::File f;
  std::string err;

  std::string full = mainsail_tar("v2");
  TEST_ASSERT_FALSE(install(ui, full.substr(0, full.size() / 2), 4096, &err));  // cut off
  TEST_ASSERT_TRUE(err.find("incomplete") != std::string::npos);

  Tar noindex;
  noindex.entry("app.js", "x");
  TEST_ASSERT_FALSE(install(ui, noindex.done(), 4096, &err));
  TEST_ASSERT_TRUE(err.find("index.html") != std::string::npos);

  Tar evil;
  evil.entry("index.html", "x");
  evil.entry("../escape.txt", "x");
  TEST_ASSERT_FALSE(install(ui, evil.done(), 4096, &err));
  TEST_ASSERT_FALSE(exists(g_root + "/escape.txt"));

  std::string garbage(2048, 'x');
  TEST_ASSERT_FALSE(install(ui, garbage, 4096, &err));
  TEST_ASSERT_TRUE(err.find("checksum") != std::string::npos);

  TEST_ASSERT_TRUE(ui.lookup("/", f));
  TEST_ASSERT_EQUAL_STRING("GZ<html v1>", slurp(f.path).c_str());
  TEST_ASSERT_FALSE(exists(root + ".new"));

  // A good one replaces it, and files the new one lacks are gone.
  Tar v3;
  v3.entry("index.html", "<html v3>");
  TEST_ASSERT_TRUE(install(ui, v3.done(), 4096));
  TEST_ASSERT_TRUE(ui.lookup("/index.html", f));
  TEST_ASSERT_FALSE(f.gzip);
  TEST_ASSERT_EQUAL_STRING("<html v3>", slurp(f.path).c_str());
  TEST_ASSERT_FALSE(ui.lookup("/config.json", f));
  TEST_ASSERT_FALSE(exists(root + ".old"));
}

// What ./run.sh webui sends: an archive from GNU tar in ustar format, with deep paths that
// need the prefix field. Skipped where tar is missing.
void test_webui_accepts_gnu_tar() {
  std::string src = g_root + "/src";
  std::string deep = "assets/a-rather-long-directory-name-for-the-prefix-field/"
                     "and-another-rather-long-directory-name-to-pass-a-hundred";
  std::string cmd = "mkdir -p '" + src + "/" + deep + "' && printf '<html>' | gzip > '" + src +
                    "/index.html.gz' && printf 'deep' > '" + src + "/" + deep +
                    "/file-with-a-long-name.js' && tar --format=ustar -C '" + src +
                    "' -cf '" + g_root + "/ui.tar' . 2>/dev/null";
  if (system(cmd.c_str()) != 0) TEST_IGNORE_MESSAGE("no GNU tar");
  WebUi ui(g_root + "/www");
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(install(ui, slurp(g_root + "/ui.tar"), 8192, &err), err.c_str());
  WebUi::File f;
  TEST_ASSERT_TRUE(ui.lookup("/" + deep + "/file-with-a-long-name.js", f));
  TEST_ASSERT_EQUAL_STRING("deep", slurp(f.path).c_str());
  TEST_ASSERT_TRUE(ui.lookup("/", f));
  TEST_ASSERT_TRUE(f.gzip);
}

// ---- ZipToTar: Mainsail's release zip into the tar the installer takes --------------------

// A zip written the way Info-ZIP writes Mainsail's release: sizes in the local headers.
struct Zip {
  std::string data, dir;
  int count = 0;
  static void put16(std::string& s, unsigned v) { s += (char)(v & 255); s += (char)(v >> 8 & 255); }
  static void put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }
  void add(const std::string& name, const std::string& body, bool compress, uint16_t flags = 0) {
    std::string packed = body;
    if (compress) {
      z_stream z = {};
      deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
      packed.resize(deflateBound(&z, body.size()));
      z.next_in = (Bytef*)body.data();
      z.avail_in = body.size();
      z.next_out = (Bytef*)&packed[0];
      z.avail_out = packed.size();
      deflate(&z, Z_FINISH);
      packed.resize(z.total_out);
      deflateEnd(&z);
    }
    uint32_t crc = crc32(0, (const Bytef*)body.data(), body.size()), at = data.size();
    std::string h;
    put32(h, 0x04034b50); put16(h, 20); put16(h, flags); put16(h, compress ? 8 : 0);
    put16(h, 0); put16(h, 0); put32(h, crc); put32(h, packed.size()); put32(h, body.size());
    put16(h, name.size()); put16(h, 4);
    data += h + name + std::string("ab\0\0", 4) + packed;  // a 4-byte extra field, as zip tools add
    std::string c;
    put32(c, 0x02014b50); put16(c, 20); put16(c, 20); put16(c, flags); put16(c, compress ? 8 : 0);
    put16(c, 0); put16(c, 0); put32(c, crc); put32(c, packed.size()); put32(c, body.size());
    put16(c, name.size()); put16(c, 0); put16(c, 0); put16(c, 0); put16(c, 0); put32(c, 0); put32(c, at);
    dir += c + name;
    count++;
  }
  std::string done() {
    std::string e;
    put32(e, 0x06054b50); put16(e, 0); put16(e, 0); put16(e, count); put16(e, count);
    put32(e, dir.size()); put32(e, data.size()); put16(e, 0);
    return data + dir + e;
  }
};

static std::string zip_to_tar(const std::string& zip, size_t chunk, std::string* err = nullptr) {
  std::string tar;
  ZipToTar z([&](const uint8_t* d, size_t n) { tar.append((const char*)d, n); return true; });
  bool ok = true;
  for (size_t pos = 0; ok && pos < zip.size(); pos += chunk)
    ok = z.feed((const uint8_t*)zip.data() + pos, std::min(chunk, zip.size() - pos));
  ok = ok && z.finish();
  if (err) *err = z.error();
  return ok ? tar : "";
}

static std::string gunzip(const std::string& gz) {
  z_stream z = {};
  inflateInit2(&z, 16 + 15);
  std::string out(16 << 20, '\0');  // Mainsail's largest file is a few MB
  z.next_in = (Bytef*)gz.data();
  z.avail_in = gz.size();
  z.next_out = (Bytef*)&out[0];
  z.avail_out = out.size();
  int r = inflate(&z, Z_FINISH);  // checks the gzip trailer: CRC and size
  out.resize(z.total_out);
  inflateEnd(&z);
  return r == Z_STREAM_END ? out : "<bad gzip>";
}

void test_zip_to_tar_installs_mainsail() {
  std::string html = "<html>" + std::string(3000, 'x') + "</html>";
  std::string js(5000, 'j');
  for (size_t i = 0; i < js.size(); i += 7) js[i] = (char)('a' + i % 26);
  Zip zip;
  zip.add("assets/", "", false);
  zip.add("index.html", html, true);
  zip.add(".version", "v9.9.9", false);
  zip.add("assets/index-AbC.js", js, true);
  zip.add("img/logo.png", "PNG....", false);
  zip.add(std::string(80, 'd') + "/" + std::string(60, 'f') + ".js", "long", true);  // ustar prefix
  zip.add("empty.txt", "", true);
  std::string full = zip.done();
  std::string root = g_root + "/www";
  for (size_t chunk : {1, 3, 30, 512, 4096, 1000000}) {
    std::string err, tar = zip_to_tar(full, chunk, &err);
    TEST_ASSERT_FALSE_MESSAGE(tar.empty(), err.c_str());
    WebUi ui(root);
    TEST_ASSERT_TRUE(install(ui, tar, 4096, &err));
    TEST_ASSERT_EQUAL_STRING("v9.9.9", ui.version().c_str());
    WebUi::File f;
    TEST_ASSERT_TRUE(ui.lookup("/", f));
    TEST_ASSERT_TRUE(f.gzip);
    TEST_ASSERT_EQUAL_STRING(html.c_str(), gunzip(slurp(f.path)).c_str());
    TEST_ASSERT_TRUE(ui.lookup("/assets/index-AbC.js", f));
    TEST_ASSERT_EQUAL_STRING(js.c_str(), gunzip(slurp(f.path)).c_str());
    TEST_ASSERT_TRUE(ui.lookup("/img/logo.png", f));
    TEST_ASSERT_FALSE(f.gzip);
    TEST_ASSERT_EQUAL_STRING("PNG....", slurp(f.path).c_str());
    TEST_ASSERT_TRUE(ui.lookup("/" + std::string(80, 'd') + "/" + std::string(60, 'f') + ".js", f));
    TEST_ASSERT_EQUAL_STRING("long", gunzip(slurp(f.path)).c_str());
    TEST_ASSERT_TRUE(ui.lookup("/empty.txt", f));
    TEST_ASSERT_EQUAL_STRING("", gunzip(slurp(f.path)).c_str());
  }
}

void test_zip_to_tar_refusals() {
  std::string err;
  TEST_ASSERT_TRUE(zip_to_tar(std::string(100, 'x'), 7, &err).empty());
  TEST_ASSERT_TRUE(err.find("not a zip") != std::string::npos);
  Zip dd;
  dd.add("index.html", "x", true, 8);  // sizes in a data descriptor
  TEST_ASSERT_TRUE(zip_to_tar(dd.done(), 4096, &err).empty());
  TEST_ASSERT_TRUE(err.find("data descriptor") != std::string::npos);
  Zip ok;  // cut off in the middle of a file (past the files, only the directory is missing)
  ok.add("index.html", std::string(2000, 'h'), false);
  std::string full = ok.done();
  TEST_ASSERT_TRUE(zip_to_tar(full.substr(0, 1000), 4096, &err).empty());
  TEST_ASSERT_TRUE(err.find("cut off") != std::string::npos);
}

// The fork's zip from ../mainsail/dist, or "" when ./run.sh hasn't built it. The test asks
// have_real_mainsail_zip() first, so that no string is alive when TEST_IGNORE jumps out of it
// (a longjmp): ASan counts those as leaks and fails the whole suite.
std::string real_mainsail_zip(std::string& version) {
  std::string here = __FILE__;
  std::string top = here.substr(0, here.rfind('/')) + "/../../../";
  auto line = [](std::string s) { return s.substr(0, s.find_first_of("\r\n")); };
  version = line(slurp(top + "mainsail/UPSTREAM")) + "-halyard." + line(slurp(top + "mainsail/REVISION"));
  return slurp(top + "mainsail/dist/mainsail-" + version + ".zip");
}

// Halyard's Mainsail (D13), when ./run.sh has built it (../mainsail): every file gunzips (CRC
// checked), and the installed UI has its 193 files, halyard-config.html among them.
bool have_real_mainsail_zip() {
  std::string version;
  return !real_mainsail_zip(version).empty();
}

void test_zip_to_tar_real_mainsail() {
  if (!have_real_mainsail_zip()) TEST_IGNORE_MESSAGE("no mainsail/dist zip (./run.sh test builds it)");
  std::string version, zip = real_mainsail_zip(version);
  std::string err, tar = zip_to_tar(zip, 4096, &err);
  TEST_ASSERT_FALSE_MESSAGE(tar.empty(), err.c_str());
  WebUi ui(g_root + "/www");
  TEST_ASSERT_TRUE_MESSAGE(install(ui, tar, 4096, &err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(version.c_str(), ui.version().c_str());
  WebUi::File ref;
  TEST_ASSERT_TRUE(ui.lookup("/halyard-config.html", ref));
  int files = 0, gz = 0;
  std::function<void(const std::string&)> walk = [&](const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    while (struct dirent* e = readdir(d)) {
      std::string n = e->d_name, p = dir + "/" + n;
      if (n == "." || n == "..") continue;
      struct stat st;
      stat(p.c_str(), &st);
      if (S_ISDIR(st.st_mode)) { walk(p); continue; }
      files++;
      if (p.size() > 3 && p.compare(p.size() - 3, 3, ".gz") == 0) {
        gz++;
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, gunzip(slurp(p)).compare("<bad gzip>"), p.c_str());
      }
    }
    closedir(d);
  };
  walk(g_root + "/www");
  TEST_ASSERT_EQUAL(193, files);
  TEST_ASSERT_GREATER_THAN(150, gz);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_addresses);
  RUN_TEST(test_default_trusts_private_ranges);
  RUN_TEST(test_api_key_admits_the_untrusted);
  RUN_TEST(test_set_trusted);
  RUN_TEST(test_webui_install_and_lookup);
  RUN_TEST(test_webui_failed_install_keeps_the_old_one);
  RUN_TEST(test_webui_accepts_gnu_tar);
  RUN_TEST(test_webui_install_pauses_between_files);
  RUN_TEST(test_webui_keeps_the_previous_one);
  RUN_TEST(test_zip_to_tar_installs_mainsail);
  RUN_TEST(test_zip_to_tar_refusals);
  RUN_TEST(test_zip_to_tar_real_mainsail);
  return UNITY_END();
}
