// SPDX-License-Identifier: GPL-3.0-or-later
#include "web.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include "ota.h"
#include "update_page.h"
#include "updates.h"

using halyard::Access;
using halyard::HttpResponse;
using halyard::IpAddr;
using halyard::Service;
using halyard::WebUi;
using halyard::ZipStream;

namespace web {

namespace {

httpd_handle_t server = nullptr;
Service* svc = nullptr;
Access* access = nullptr;
WebUi* ui = nullptr;
SemaphoreHandle_t lock = nullptr;
std::atomic<bool> served{false};  // a request has been answered (an OTA image proves itself)
std::atomic<bool> flush_queued{false};

const size_t kChunk = 8192;

void send_queued();

// esp_http_server has one task, so the websocket pushes flush() queues wait while a handler
// moves a large file. Mainsail closes a websocket that has been silent for 10 s: a 7 MB
// upload or download showed "connection failed" (found on hardware). Long loops call this
// to send them on the way.
TickType_t last_keep_alive = 0;
void keep_alive() {
  if (xTaskGetTickCount() - last_keep_alive < pdMS_TO_TICKS(250)) return;
  last_keep_alive = xTaskGetTickCount();
  send_queued();
}
const int kMaxRecvTimeouts = 30;  // x recv_wait_timeout: a stalled upload is given up

// Only the server's task takes this. Arduino's SD driver busy-waits, and a run of requests
// that each read the card (a browser loading Mainsail with an empty cache asks for about a
// hundred files, and each lookup scans a folder of 180) can keep core 0 busy for seconds.
// The idle task then starves and the task watchdog resets Halyard (found on hardware). So
// after 100 ms without a pause, letting go of the lock also gives the idle task a tick.
TickType_t last_pause = 0;
int depth = 0;

struct Locked {
  Locked() {
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    depth++;
  }
  ~Locked() {
    xSemaphoreGiveRecursive(lock);
    if (--depth == 0 && xTaskGetTickCount() - last_pause >= pdMS_TO_TICKS(100)) {
      vTaskDelay(1);
      last_pause = xTaskGetTickCount();
    }
  }
};

const char* status_line(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

// Mainsail served from another origin (the PC, or my.mainsail.xyz) calls the API directly.
void cors(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Api-Key, Authorization");
}

esp_err_t send(httpd_req_t* req, const HttpResponse& r) {
  httpd_resp_set_status(req, status_line(r.status));
  httpd_resp_set_type(req, r.content_type.c_str());
  cors(req);
  return httpd_resp_send(req, r.body.data(), r.body.size());
}

esp_err_t send_error(httpd_req_t* req, int status, const char* msg) {
  HttpResponse r;
  r.status = status;
  r.body = std::string("{\"error\":{\"code\":") + std::to_string(status) + ",\"message\":\"" + msg + "\"}}";
  return send(req, r);
}

std::string header(httpd_req_t* req, const char* name) {
  size_t n = httpd_req_get_hdr_value_len(req, name);
  if (!n) return "";
  std::string v(n + 1, '\0');
  httpd_req_get_hdr_value_str(req, name, &v[0], n + 1);
  v.resize(n);
  return v;
}

void split_uri(httpd_req_t* req, std::string& path, std::string& query) {
  const char* q = strchr(req->uri, '?');
  path = q ? std::string(req->uri, q - req->uri) : std::string(req->uri);
  query = q ? std::string(q + 1) : std::string();
}

// The peer's address, IPv4 as IPv4-mapped. The server's socket is IPv6 (lwIP maps IPv4
// clients), but both families are handled.
IpAddr peer(httpd_req_t* req) {
  IpAddr a;
  struct sockaddr_storage ss = {};
  socklen_t len = sizeof ss;
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr*)&ss, &len) != 0) return a;
  if (ss.ss_family == AF_INET) {
    const uint8_t* b = (const uint8_t*)&((struct sockaddr_in*)&ss)->sin_addr.s_addr;
    return IpAddr::v4(b[0], b[1], b[2], b[3]);
  }
  if (ss.ss_family == AF_INET6) memcpy(a.b, &((struct sockaddr_in6*)&ss)->sin6_addr, 16);
  return a;
}

// Moonraker's rule: a trusted address, or the API key in X-Api-Key.
bool authorized(httpd_req_t* req) {
  IpAddr a = peer(req);
  std::string key = header(req, "X-Api-Key");
  Locked l;
  return access->allowed(a, key);
}

const char kUntrusted[] =
    "<!doctype html><meta charset=utf-8><title>Halyard</title>"
    "<body style='font-family:sans-serif;max-width:40em;margin:2em auto;padding:0 1em'>"
    "<h1>Halyard</h1><p>This address is not one Halyard trusts. On Halyard's USB console, "
    "<code>trust</code> shows the trusted ranges and <code>trust add</code> adds one. Slicers "
    "and scripts can send the API key (console: <code>apikey</code>) in an X-Api-Key header "
    "instead.</p>";

esp_err_t unauthorized(httpd_req_t* req, bool page) {
  if (!page) return send_error(req, 401, "Unauthorized");
  httpd_resp_set_status(req, status_line(401));
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kUntrusted, sizeof kUntrusted - 1);
}

const char* method_name(int m) {
  switch (m) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_DELETE: return "DELETE";
    default: return "OTHER";
  }
}

// Reads the request body, calling `chunk` for each piece as it arrives. False if the
// client went away or stalled.
template <typename F>
bool read_body(httpd_req_t* req, F chunk) {
  std::unique_ptr<char[]> buf(new char[kChunk]);
  size_t left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    int n = httpd_req_recv(req, buf.get(), left < kChunk ? left : kChunk);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxRecvTimeouts) continue;
    if (n <= 0) return false;
    timeouts = 0;
    left -= n;
    chunk((const uint8_t*)buf.get(), (size_t)n);
    keep_alive();
  }
  return true;
}

// ---- websocket -------------------------------------------------------------------------

void send_pending(void*) {
  flush_queued = false;
  send_queued();
}

// On the server's task only. It leaves flush_queued alone, so flush() doesn't pile up work
// for the task while keep_alive() sends.
void send_queued() {
  for (;;) {
    int conn;
    std::string text;
    {
      Locked l;
      if (!svc->take(conn, text)) return;
    }
    if (httpd_ws_get_fd_info(server, conn) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.final = true;
    f.payload = (uint8_t*)text.data();
    f.len = text.size();
    httpd_ws_send_frame_async(server, conn, &f);
  }
}

esp_err_t ws_handler(httpd_req_t* req) {
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET) {  // the handshake (IDF has answered it already)
    if (!authorized(req)) return ESP_FAIL;  // closes the socket: Mainsail sees a failure
    served = true;
    Locked l;
    svc->ws_open(fd);
    return ESP_OK;
  }
  httpd_ws_frame_t f = {};
  esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
  if (e != ESP_OK) return e;
  if (f.len > 256 * 1024) return ESP_FAIL;
  std::string text(f.len, '\0');
  if (f.len) {
    f.payload = (uint8_t*)&text[0];
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e != ESP_OK) return e;
  }
  if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
  std::string reply;
  {
    Locked l;
    reply = svc->ws_message(fd, text);
  }
  if (!reply.empty()) {
    httpd_ws_frame_t out = {};
    out.type = HTTPD_WS_TYPE_TEXT;
    out.final = true;
    out.payload = (uint8_t*)reply.data();
    out.len = reply.size();
    e = httpd_ws_send_frame(req, &out);
  }
  flush();
  return e;
}

// With close_fn set, esp_http_server leaves closing the socket to it (as Espressif's wss example
// does). Without the close(), every closed connection leaked a socket until accept() failed.
void on_close(httpd_handle_t, int fd) {
  {
    Locked l;
    svc->ws_close(fd);
  }
  close(fd);
}

// ---- uploads and downloads -------------------------------------------------------------

esp_err_t upload_handler(httpd_req_t* req) {
  if (!authorized(req)) return unauthorized(req, false);
  bool octoprint = strncmp(req->uri, "/api/", 5) == 0;
  HttpResponse out;
  std::unique_ptr<Service::Upload> up;
  {
    Locked l;
    up = svc->upload(octoprint, header(req, "Content-Type"), out);
  }
  if (up) {
    bool going = true;
    // After a refusal the rest of the body is read and dropped, so the reply is not lost.
    bool whole = read_body(req, [&](const uint8_t* d, size_t n) {
      if (!going) return;
      Locked l;
      going = up->feed(d, n);
    });
    Locked l;
    up->finish(out);
    up.reset();  // deletes an unfinished file, on the SD card: under the lock
    if (!whole) return ESP_FAIL;
  }
  flush();
  return send(req, out);
}

const char* content_type_for(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (auto& c : ext) c = (char)tolower((unsigned char)c);
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gcode" || ext == "gco" || ext == "g") return "text/plain";
  return "application/octet-stream";
}

// Sends an open file in chunks, reading under the lock (the card is shared) and sending
// without it. Closes the file.
esp_err_t send_file(httpd_req_t* req, FILE* f) {
  std::unique_ptr<char[]> buf(new char[kChunk]);
  esp_err_t e = ESP_OK;
  for (;;) {
    size_t n;
    {
      Locked l;
      n = fread(buf.get(), 1, kChunk, f);
    }
    if (n == 0) break;
    e = httpd_resp_send_chunk(req, buf.get(), n);
    if (e != ESP_OK) break;
    keep_alive();
  }
  {
    Locked l;
    fclose(f);
  }
  if (e == ESP_OK) e = httpd_resp_send_chunk(req, nullptr, 0);
  return e;
}

// The archive server.files.zip promised, made as it goes out. A file that can't be read
// ends the connection without the last chunk, so the browser reports a failed download.
esp_err_t send_zip(httpd_req_t* req, std::unique_ptr<ZipStream> zip, const std::string& rel) {
  std::string disposition = Service::content_disposition(rel);
  httpd_resp_set_type(req, "application/zip");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cors(req);
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
  esp_err_t e = ESP_OK;
  for (;;) {
    size_t n;
    {
      Locked l;
      n = zip->read(buf.get(), kChunk);
    }
    if (n == 0) break;
    e = httpd_resp_send_chunk(req, (const char*)buf.get(), n);
    if (e != ESP_OK) break;
    keep_alive();
  }
  bool failed = zip->failed();
  {
    Locked l;
    zip.reset();  // closes the file it was reading
  }
  if (failed) return ESP_FAIL;
  if (e == ESP_OK) e = httpd_resp_send_chunk(req, nullptr, 0);
  return e;
}

esp_err_t download(httpd_req_t* req, const std::string& rel) {
  std::string full, body;
  FILE* f = nullptr;
  bool made;
  std::unique_ptr<ZipStream> zip;
  {
    Locked l;
    zip = svc->zip_download(rel);
    made = !zip && svc->download_text(rel, body);
    if (!zip && !made) full = svc->download_path(rel);
    if (!full.empty()) f = fopen(full.c_str(), "rb");
  }
  if (zip) return send_zip(req, std::move(zip), rel);
  // Kept until the reply is sent: httpd_resp_set_hdr keeps the pointer.
  std::string disposition = Service::content_disposition(made ? rel : full);
  if (made || f) httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
  if (made) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    cors(req);
    return httpd_resp_send(req, body.data(), body.size());
  }
  if (!f) return send_error(req, 404, "Not Found");
  httpd_resp_set_type(req, content_type_for(full));
  cors(req);
  return send_file(req, f);
}

// Mainsail from the card, stored gzipped (webui.h). False if there is no such file.
bool serve_ui(httpd_req_t* req, const std::string& path, esp_err_t& e) {
  WebUi::File file;
  FILE* f = nullptr;
  {
    Locked l;
    if (ui->lookup(path, file)) f = fopen(file.path.c_str(), "rb");
  }
  if (!f) return false;
  httpd_resp_set_type(req, file.content_type.c_str());
  if (file.gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control",
                     file.immutable ? "public, max-age=31536000, immutable" : "no-cache");
  e = send_file(req, f);
  return true;
}

// ---- installing Mainsail and firmware (./run.sh webui, ./run.sh ota) ----------------------

// POST /halyard/webui: a tar of the gzipped UI. It replaces the old UI only when complete.
esp_err_t webui_handler(httpd_req_t* req) {
  if (!authorized(req)) return unauthorized(req, false);
  std::unique_ptr<WebUi::Install> in;
  {
    Locked l;
    in = ui->begin_install();
  }
  bool going = true;
  bool whole = read_body(req, [&](const uint8_t* d, size_t n) {
    if (!going) return;
    Locked l;
    going = in->feed(d, n);
  });
  bool ok;
  std::string msg;
  {
    Locked l;
    ok = whole && in->finish();
    msg = in->error();
    if (ok)
      msg = std::to_string(in->files()) + " files, " + std::to_string(in->bytes()) + " bytes";
    in.reset();  // removes an unfinished install, on the card: under the lock
  }
  if (!whole) return ESP_FAIL;
  if (!ok) return send_error(req, 400, msg.c_str());
  HttpResponse r;
  r.body = "{\"result\":\"ok\",\"installed\":\"" + msg + "\"}";
  return send(req, r);
}

// POST /halyard/webui/restore: back to the Mainsail the last install replaced.
esp_err_t restore_handler(httpd_req_t* req) {
  if (!authorized(req)) return unauthorized(req, false);
  std::string err, now;
  bool ok;
  {
    Locked l;
    ok = ui->restore_previous(err);
    now = ui->version();
  }
  if (!ok) return send_error(req, 400, err.c_str());
  HttpResponse r;
  r.body = "{\"result\":\"ok\",\"mainsail\":\"" + now + "\"}";
  return send(req, r);
}

void restart_soon() {
  Locked l;
  svc->reboot_soon();
}

// POST /halyard/update: a firmware.bin, written to the other OTA slot. The new image boots on
// trial: it is kept once it has answered a request over the network (main.cpp), and the
// bootloader goes back to this one if it resets before that.
esp_err_t update_handler(httpd_req_t* req) {
  if (!authorized(req)) return unauthorized(req, false);
  std::string why;
  bool can;
  {
    Locked l;
    can = svc->can_restart(why);
  }
  if (!can) return send_error(req, 409, why.c_str());
  if (req->content_len == 0) return send_error(req, 413, "not a firmware image of a size that fits");
  ota::Writer w;
  if (!w.begin(req->content_len)) return send_error(req, w.status(), w.error().c_str());
  bool going = true;
  bool whole = read_body(req, [&](const uint8_t* d, size_t n) {
    if (going) going = w.write(d, n);
  });
  if (!whole) return ESP_FAIL;
  if (!going || !w.finish()) return send_error(req, w.status(), w.error().c_str());
  HttpResponse r;
  r.body = std::string("{\"result\":\"ok\",\"partition\":\"") + w.partition() +
           "\",\"restarting\":true}";
  esp_err_t e = send(req, r);
  restart_soon();
  return e;
}

// ---- everything else -------------------------------------------------------------------

const char kIndexHead[] =
    "<!doctype html><meta charset=utf-8><title>Halyard</title>"
    "<body style='font-family:sans-serif;max-width:40em;margin:2em auto;padding:0 1em'>"
    "<h1>Halyard</h1><p>A print server for Marlin printers. It speaks Moonraker's API for "
    "Mainsail or Fluidd, and OctoPrint's upload API for slicers.</p>"
    "<p><b>Mainsail is not on the card yet.</b> ";
const char kIndexTail[] =
    "</p><p>It can also be installed from a <code>mainsail.zip</code> on the "
    "<a href='/halyard/'>update page</a>. In a slicer, add a physical printer of type "
    "OctoPrint with this address.</p>";

std::string html_text(const std::string& s) {
  std::string o;
  for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
  return o;
}

const char kNoCardIndex[] =
    "<!doctype html><meta charset=utf-8><title>Halyard</title>"
    "<body style='font-family:sans-serif;max-width:40em;margin:2em auto;padding:0 1em'>"
    "<h1>Halyard</h1><p><b>No SD card it can read.</b> Halyard keeps its settings, its "
    "library and Mainsail on a microSD card in the GEEK. It must be FAT32: 32 GB or smaller, "
    "since larger cards come formatted as exFAT. Prepare it on a PC (see Halyard's README), "
    "put it in and restart Halyard.</p><p><a href='/halyard/'>Firmware updates</a> work "
    "without a card.</p>";

esp_err_t api_handler(httpd_req_t* req) {
  if (req->method == HTTP_OPTIONS) {  // CORS preflight
    httpd_resp_set_status(req, status_line(204));
    cors(req);
    return httpd_resp_send(req, nullptr, 0);
  }
  std::string path, query, body;
  split_uri(req, path, query);
  bool api = path.compare(0, 5, "/api/") == 0 || path.compare(0, 8, "/server/") == 0 ||
             path.compare(0, 9, "/printer/") == 0 || path.compare(0, 9, "/machine/") == 0 ||
             path.compare(0, 8, "/access/") == 0;
  if (!authorized(req)) return unauthorized(req, !api);
  served = true;
  if (req->method == HTTP_GET &&
      (path.compare(0, 21, "/server/files/gcodes/") == 0 ||
       path.compare(0, 21, "/server/files/config/") == 0))
    return download(req, path.substr(14));
  if (req->content_len > 64 * 1024) return send_error(req, 413, "Payload Too Large");
  if (!read_body(req, [&](const uint8_t* d, size_t n) { body.append((const char*)d, n); }))
    return ESP_FAIL;
  HttpResponse out;
  bool found;
  {
    Locked l;
    found = svc->http(method_name(req->method), path, query, body, out);
  }
  flush();
  if (found) return send(req, out);
  if (req->method == HTTP_GET) {
    esp_err_t e;
    if (path == "/halyard/info") {  // for the update page
      HttpResponse r;
      {
        Locked l;
        r.body = "{\"mainsail\":\"" + ui->version() + "\",\"previous\":\"" + ui->previous_version() + "\"}";
      }
      r.content_type = "application/json";
      return send(req, r);
    }
    if (path == "/halyard" || path == "/halyard/") {
      httpd_resp_set_type(req, "text/html");
      httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
      return httpd_resp_send(req, kUpdatePage, sizeof kUpdatePage - 1);
    }
    if (serve_ui(req, path, e)) return e;
    if (path == "/" || path == "/index.html") {
      bool no_card;
      {
        Locked l;
        no_card = svc->card_missing();
      }
      httpd_resp_set_type(req, "text/html");
      if (no_card) return httpd_resp_send(req, kNoCardIndex, sizeof kNoCardIndex - 1);
      // What the automatic install is doing (updates.cpp); it waits 30 s after WiFi comes up.
      std::string st = updates::status();
      if (st.empty()) st = "Halyard fetches it by itself shortly after it joins the network.";
      std::string page = kIndexHead + html_text(st) + kIndexTail;
      httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
      return httpd_resp_send(req, page.data(), page.size());
    }
  }
  return send_error(req, 404, "Not Found");
}

}  // namespace

void flush() {
  if (!server) return;
  bool expected = false;
  if (!flush_queued.compare_exchange_strong(expected, true)) return;
  if (httpd_queue_work(server, send_pending, nullptr) != ESP_OK) flush_queued = false;
}

bool served_any() { return served; }

bool begin(Service& s, Access& a, WebUi& w, SemaphoreHandle_t core_lock) {
  svc = &s;
  access = &a;
  ui = &w;
  lock = core_lock;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.core_id = 0;               // the printer loop has core 1
  cfg.stack_size = 12 * 1024;    // JSON documents are built on this task
  cfg.max_open_sockets = 10;     // a browser's 6, the websocket, and room to spare (16 in lwIP)
  cfg.max_uri_handlers = 14;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 10;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.close_fn = on_close;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    server = nullptr;
    return false;
  }
  // First match wins, so the specific routes come before the catch-alls.
  httpd_uri_t ws = {};
  ws.uri = "/websocket";
  ws.method = HTTP_GET;
  ws.handler = ws_handler;
  ws.is_websocket = true;
  httpd_register_uri_handler(server, &ws);
  httpd_uri_t up = {};
  up.method = HTTP_POST;
  up.uri = "/halyard/update";
  up.handler = update_handler;
  httpd_register_uri_handler(server, &up);
  up.uri = "/halyard/webui";
  up.handler = webui_handler;
  httpd_register_uri_handler(server, &up);
  up.uri = "/halyard/webui/restore";
  up.handler = restore_handler;
  httpd_register_uri_handler(server, &up);
  for (const char* uri : {"/server/files/upload", "/api/files/local"}) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = HTTP_POST;
    u.handler = upload_handler;
    httpd_register_uri_handler(server, &u);
  }
  for (httpd_method_t m : {HTTP_GET, HTTP_POST, HTTP_DELETE, HTTP_OPTIONS}) {
    httpd_uri_t u = {};
    u.uri = "/*";
    u.method = m;
    u.handler = api_handler;
    httpd_register_uri_handler(server, &u);
  }
  return true;
}

}  // namespace web
