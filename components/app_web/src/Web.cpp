/*
   HTTP server: REST API and the embedded UI.
*/

#include "appweb/Web.h"
#include "Tls.h"
#include "appcfg/Config.h"
#include "appnet/Net.h"
#include "appoh/OpenHab.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_ota_ops.h>
#include <esp_rom_sys.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <cinttypes>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <cJSON.h>
#include <qrcodegen.h>

#include <lwip/sockets.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

/* Generated from www/index.html and www/layout.json by tools/gen_ui_asset.py, see CMakeLists.txt. */
extern "C" {
extern const unsigned char ui_index_html_gz[];
extern const unsigned int ui_index_html_gz_len;
extern const unsigned char ui_layout_json_gz[];
extern const unsigned int ui_layout_json_gz_len;
}

#include "WebDetail.h"

namespace appweb {
namespace detail {

httpd_handle_t g_server = nullptr;

httpd_handle_t g_tls_server = nullptr;   // the same on port 443, with the panel's certificate (Tls.h)

OtaHook g_ota_hook;

PanelApi g_panel;

DiagnosticsApi g_diag;

DisplayApi g_display;

/* Lets the application signal an update without this component knowing how. */
void indicator_hook(bool active) {
  if (g_ota_hook)
    g_ota_hook(active);
}

/* The server's sockets (max_open_sockets below). */
constexpr size_t kMaxSockets = 7;

/* Set as soon as any request reaches the server. A new firmware image is on
   trial until something proves it works, and being asked for a page proves it
   as well as joining a network does -- better, in the case that matters, which
   is a device serving its own setup network with somebody standing in front of
   it. */
std::atomic<bool> g_served{false};

// ---------------------------------------------------------------- auth ----

/* Protection applies everywhere, including the fallback access point.

   It was exempt there at first, reasoning that a password would lock the user
   out of a device they cannot reach any other way. That reasoning does not
   survive scrutiny: jamming the WiFi is enough to force the device into
   access-point mode, and an open setup network then hands over every setting to
   whoever is nearby.

   The lockout it was meant to prevent is covered by the recovery gesture on
   the panel's buttons, which erases the WiFi credentials *and* the password and
   needs physical access and nothing else (see recovery.h). */
bool auth_required() { return appcfg::auth().enabled; }

/* Verifying a password costs 20 000 rounds of PBKDF2, which is the right price
   for a login and the wrong one for a request. Basic authentication sends the
   header on every single request, and the interface polls several times a
   minute: without this the device would spend most of its httpd task deriving
   the same key over and over, and anyone could make it do that by sending
   requests.

   So a header that has already been verified is remembered by its hash for a
   few minutes. Only credentials that passed can ever be in here, so the cache
   cannot admit anyone; it only postpones noticing a password change, which is
   why changing one clears it. */
constexpr int64_t kAuthCacheUs = 300 * 1000000LL;

uint8_t g_auth_hash[32] = {};

bool g_auth_valid = false;

int64_t g_auth_until = 0;

/* A wrong password is cheap for the sender and expensive for us, so after a
   handful of failures the key derivation is skipped entirely for a while. The
   window is short enough to be invisible to somebody mistyping a password and
   long enough to make guessing pointless. */
constexpr int kMaxAuthFailures = 5;

constexpr int64_t kAuthBackoffUs = 10 * 1000000LL;

int g_auth_failures = 0;

int64_t g_auth_blocked_until = 0;

// the http and the https server each run a task of their own: the cache above is shared by both
std::mutex g_auth_mutex;

/* Same hash primitive the password derivation uses, through the same API, so
   this file does not pull in a second crypto interface. A failure leaves the
   digest zeroed, which simply misses the cache and costs a derivation. */
void hash_header(const std::string &header, uint8_t out[32]) {
  std::memset(out, 0, 32);
  size_t written = 0;
  psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(header.data()),
                   header.size(), out, 32, &written);
}

void forget_auth_cache() {
  std::lock_guard<std::mutex> lock(g_auth_mutex);
  g_auth_valid = false;
  g_auth_failures = 0;
  g_auth_blocked_until = 0;
}

bool check_auth(httpd_req_t *req) {
  if (!auth_required())
    return true;

  size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len == 0 || len > 256)
    return false;

  std::string header(len + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, "Authorization", header.data(), len + 1) != ESP_OK)
    return false;
  header.resize(len);

  const int64_t now = esp_timer_get_time();
  uint8_t digest[32];
  hash_header(header, digest);
  {
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    if (g_auth_valid && now < g_auth_until && std::memcmp(digest, g_auth_hash, sizeof(digest)) == 0)
      return true;
    if (g_auth_failures >= kMaxAuthFailures && now < g_auth_blocked_until)
      return false;
  }

  const std::string prefix = "Basic ";
  if (header.rfind(prefix, 0) != 0)
    return false;

  const std::string b64 = header.substr(prefix.size());
  unsigned char decoded[256];
  size_t decoded_len = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                            reinterpret_cast<const unsigned char *>(b64.data()), b64.size()) != 0)
    return false;
  decoded[decoded_len] = '\0';

  std::string creds(reinterpret_cast<char *>(decoded), decoded_len);
  const size_t colon = creds.find(':');
  if (colon == std::string::npos)
    return false;

  const bool right = appcfg::verify_password(creds.substr(0, colon), creds.substr(colon + 1));   // slow: unlocked
  std::lock_guard<std::mutex> lock(g_auth_mutex);
  if (!right) {
    if (++g_auth_failures >= kMaxAuthFailures)
      g_auth_blocked_until = now + kAuthBackoffUs;
    return false;
  }
  std::memcpy(g_auth_hash, digest, sizeof(g_auth_hash));
  g_auth_valid = true;
  g_auth_until = now + kAuthCacheUs;
  g_auth_failures = 0;
  return true;
}

esp_err_t deny(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ohpanel\"");
  httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
  return ESP_OK;
}

esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, text);
  cJSON_free(text);
  return ESP_OK;
}

/* Reads a bounded request body. Anything larger is a mistake or an attack, not
   a configuration. */
bool read_body(httpd_req_t *req, std::string &out, size_t limit) {
  if (req->content_len == 0 || req->content_len > limit)
    return false;
  out.resize(req->content_len);
  size_t got = 0;
  int timeouts = 0;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, out.data() + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > kMaxRecvTimeouts)
        return false;
      continue;
    }
    timeouts = 0;
    if (r <= 0)
      return false;
    got += r;
  }
  return true;
}

/* One query parameter by name, bounded. Absent or oversized reads as empty,
   which every caller treats as "not given". */
std::string query_param(httpd_req_t *req, const char *key) {
  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 160)
    return {};
  std::string query(qlen + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), qlen + 1) != ESP_OK)
    return {};
  char value[80] = "";
  if (httpd_query_key_value(query.c_str(), key, value, sizeof(value)) != ESP_OK)
    return {};

  /* httpd_query_key_value hands back the raw slice, percent escapes and all,
     so they are decoded here. */
  std::string out;
  for (size_t i = 0; value[i]; i++) {
    if (value[i] == '+') {
      out += ' ';
    } else if (value[i] == '%' && value[i + 1] && value[i + 2]) {
      const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(value[i + 1]), lo = nib(value[i + 2]);
      if (hi < 0 || lo < 0) {
        out += value[i];  /* Not an escape after all; take it literally. */
        continue;
      }
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += value[i];
    }
  }
  return out;
}

Transfer g_tx;

const char *const kTransferKinds[] = {"", "firmware", "display"};


void tx_begin(int kind, size_t total) {
  g_tx.error.clear();
  g_tx.kind = kind;
  g_tx.done = 0;
  g_tx.total = total;
  g_tx.started_us = esp_timer_get_time();
  g_tx.running = true;
}

void tx_progress(size_t done) { g_tx.done = done; }

void tx_end(bool ok, const std::string &error) {
  if (!g_tx.running)
    return;
  g_tx.ok = ok;
  g_tx.ended_us = esp_timer_get_time();
  g_tx.running = false;
  g_tx.error = ok ? "" : error;
}

void add_transfer(cJSON *parent) {
  cJSON *t = cJSON_AddObjectToObject(parent, "transfer");
  const int64_t now = esp_timer_get_time();
  const bool running = g_tx.running;
  cJSON_AddBoolToObject(t, "running", running);
  cJSON_AddStringToObject(t, "kind", kTransferKinds[g_tx.kind]);
  cJSON_AddNumberToObject(t, "done", g_tx.done);
  cJSON_AddNumberToObject(t, "total", g_tx.total);
  cJSON_AddNumberToObject(t, "elapsed_ms", ((running ? now : g_tx.ended_us.load()) - g_tx.started_us) / 1000);
  if (!running && g_tx.kind) {
    cJSON_AddBoolToObject(t, "ok", g_tx.ok);
    cJSON_AddNumberToObject(t, "ended_ago_ms", (now - g_tx.ended_us) / 1000);
    cJSON_AddStringToObject(t, "error", g_tx.error.c_str());
  }
}

// ------------------------------------------------------------ handlers ----

/* A browser asks for this on every page: an empty answer -- without it the
   catch-all redirect (not_found) would serve the whole interface again. */
esp_err_t get_favicon(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

/* The interface's page over http, "Always use https" on: to the same address over https (307: the method kept). */
esp_err_t send_to_https(httpd_req_t *req) {
  char host[96] = "";
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !*host)
    snprintf(host, sizeof(host), "%s", appnet::ip().c_str());
  std::string h = host;
  if (h.front() != '[')   // name or IPv4, maybe with the port of http: that of https is 443
    h = h.substr(0, h.find(':'));
  else
    h = h.substr(0, h.find(']') + 1);
  const std::string to = "https://" + h + req->uri;
  httpd_resp_set_status(req, "307 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", to.c_str());
  return httpd_resp_send(req, nullptr, 0);
}

/* A response the browser caches and asks again each time (no-cache) with `etag`: true once a 304 answered it,
   the browser's copy being current. */
bool not_modified(httpd_req_t *req, const char *etag) {
  char seen[16] = "";
  httpd_resp_set_hdr(req, "ETag", etag);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", seen, sizeof(seen)) != ESP_OK || strcmp(seen, etag) != 0)
    return false;
  httpd_resp_set_status(req, "304 Not Modified");
  httpd_resp_send(req, nullptr, 0);
  return true;
}

/* An embedded gzipped file, cached by the browser and asked again each time (no-cache) with its ETag -- the file's
   own checksum, worked out once into `etag`: unchanged it is a 304 and nothing more, not 100 KB again (over https
   some seconds on this chip). */
esp_err_t send_gzipped(httpd_req_t *req, const unsigned char *data, unsigned int len, const char *type,
                       char (&etag)[12]) {
  if (!etag[0])
    snprintf(etag, sizeof(etag), "\"%08" PRIx32 "\"", esp_rom_crc32_le(0, data, len));
  if (not_modified(req, etag))
    return ESP_OK;
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, reinterpret_cast<const char *>(data), len);
}

esp_err_t get_index(httpd_req_t *req) {
  if (req->handle == g_server && tls::force_https() && !appnet::provisioning())   // before any password goes over it
    return send_to_https(req);
  /* The page itself carries no data -- everything comes from /api/v1/info,
     which is protected. Gating it anyway means the browser asks for credentials
     when the page is opened, so every later request already carries them. Left
     open, the page would load and then fail with 401 in the background, which
     browsers do not turn into a login prompt. */
  if (!check_auth(req))
    return deny(req);

  static char etag[12] = "";
  return send_gzipped(req, ui_index_html_gz, ui_index_html_gz_len, "text/html", etag);
}

/* The display's page layouts for the web preview (www/layout.json, boards/nspanel/nextion/tools/hmi2layout.py):
   fonts, objects, pictures. No login needed, like the icon set -- nothing in it is the house's. */
esp_err_t get_layout(httpd_req_t *req) {
  static char etag[12] = "";
  return send_gzipped(req, ui_layout_json_gz, ui_layout_json_gz_len, "application/json", etag);
}

esp_err_t get_info(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device", appcfg::device().name.c_str());
  cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
  cJSON_AddNumberToObject(root, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(root, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(root, "heap_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
  // in pieces, a task stack or a TLS record may not fit although enough is free
  cJSON_AddNumberToObject(root, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  /* The CPU: its clock, and per core the microseconds its idle task ran
     against the time since the start -- the load over a while is the
     difference of two looks (FreeRTOS run-time stats, sdkconfig.defaults). */
  cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
  cJSON_AddNumberToObject(cpu, "mhz", esp_rom_get_cpu_ticks_per_us());
  cJSON_AddNumberToObject(cpu, "time_us", static_cast<double>(esp_timer_get_time()));
  cJSON *idle = cJSON_AddArrayToObject(cpu, "idle_us");
  for (int core = 0; core < portNUM_PROCESSORS; core++) {
    TaskStatus_t st;
    vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &st, pdFALSE, eInvalid);  // no stack measuring
    cJSON_AddItemToArray(idle, cJSON_CreateNumber(static_cast<double>(st.ulRunTimeCounter)));
  }
  /* Zero when the PSRAM was not found -- the board is supposed to have 2 MB,
     so this is the first place to look if it does not. */
  cJSON_AddNumberToObject(root, "psram", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
  // the relays as they are switched (the web preview's buttons on home)
  cJSON *relays = cJSON_AddArrayToObject(root, "relays");
  for (int i = 0; i < 2; i++)
    cJSON_AddItemToArray(relays, cJSON_CreateBool(appcfg::relay(i)));

  cJSON *net = cJSON_AddObjectToObject(root, "net");
  cJSON_AddStringToObject(net, "state", appnet::to_string(appnet::state()));
  cJSON_AddNumberToObject(net, "channel", appnet::channel());
  cJSON_AddStringToObject(net, "ip", appnet::ip().c_str());
  cJSON_AddStringToObject(net, "ssid", appnet::ssid().c_str());
  cJSON_AddNumberToObject(net, "rssi", appnet::rssi());
  cJSON_AddBoolToObject(net, "provisioning", appnet::provisioning());
  // the address as set (ip_config) and as it is (mode, gateway, ...): from DHCP, fixed, or DHCP for now (fallback)
  add_ip_json(cJSON_AddObjectToObject(net, "ip_config"), appcfg::ip());
  {
    const appnet::Address a = appnet::address();
    cJSON_AddStringToObject(net, "mode", a.fixed ? "static" : "dhcp");
    cJSON_AddBoolToObject(net, "fallback", a.fallback);
    cJSON_AddNumberToObject(net, "prefix", a.prefix);
    cJSON_AddStringToObject(net, "gateway", a.gateway.c_str());
    cJSON_AddStringToObject(net, "dns1", a.dns1.c_str());
    cJSON_AddStringToObject(net, "dns2", a.dns2.c_str());
    cJSON_AddStringToObject(net, "dns3", a.dns3.c_str());
    cJSON_AddStringToObject(net, "gateway6", appnet::ipv6_router().c_str());
    cJSON *ts = cJSON_AddArrayToObject(net, "time_servers");
    for (const appnet::TimeServer &t : appnet::time_servers()) {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "server", t.server.c_str());
      cJSON_AddStringToObject(e, "source", t.source);
      cJSON_AddItemToArray(ts, e);
    }
    cJSON *v6 = cJSON_AddArrayToObject(net, "ipv6");
    for (const appnet::Address6 &x : appnet::ipv6()) {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "address", x.address.c_str());
      cJSON_AddStringToObject(e, "kind", x.kind);
      cJSON_AddItemToArray(v6, e);
    }
  }
  // https (Tls.h): whether it runs, whether http is sent to it -- the page offers it when opened over http
  cJSON *https = cJSON_AddObjectToObject(root, "https");
  cJSON_AddBoolToObject(https, "running", g_tls_server != nullptr);
  cJSON_AddBoolToObject(https, "force", tls::force_https());
  /* the network in the settings, also while it is out of reach and the
     hotspot is open instead (ssid then names the hotspot) */
  cJSON_AddStringToObject(net, "configured_ssid", appcfg::wifi().ssid.c_str());
  cJSON_AddStringToObject(net, "security", appcfg::wifi().wpa3_only ? "wpa3" : "auto");  // the panel's setting
  cJSON_AddStringToObject(net, "network_security", appnet::security());  // the network's, while joined
  /* the setup hotspot opens when the home network is out of reach; its name
     is the device name, and it is open */

  const appnet::Time t = appnet::time_info();
  cJSON *clock = cJSON_AddObjectToObject(root, "time");
  cJSON_AddBoolToObject(clock, "synced", t.synced);
  if (t.synced)
    cJSON_AddNumberToObject(clock, "epoch_ms", static_cast<double>(t.epoch_ms));
  const appcfg::Clock cc = appcfg::clock();
  cJSON_AddStringToObject(clock, "ntp_server", cc.server.c_str());
  cJSON_AddStringToObject(clock, "tz_name", cc.tz_name.c_str());

  const appcfg::OpenHab oh = appcfg::openhab();
  const appoh::Status ohs = appoh::status();
  cJSON *ohj = cJSON_AddObjectToObject(root, "openhab");
  cJSON_AddStringToObject(ohj, "url", oh.url.c_str());
  cJSON_AddBoolToObject(ohj, "connected", ohs.connected);
  cJSON_AddNumberToObject(ohj, "subscribed", ohs.subscribed);
  cJSON_AddNumberToObject(ohj, "received", ohs.received);
  cJSON_AddNumberToObject(ohj, "reconnects", ohs.reconnects);
  cJSON_AddStringToObject(ohj, "last_error", ohs.last_error.c_str());

  cJSON *auth = cJSON_AddObjectToObject(root, "auth");
  appcfg::Auth a = appcfg::auth();
  cJSON_AddBoolToObject(auth, "enabled", a.enabled);
  cJSON_AddBoolToObject(auth, "has_password", a.has_password());
  cJSON_AddStringToObject(auth, "user", a.user.c_str());

  if (g_diag.info_extra)
    g_diag.info_extra(root);
  add_transfer(root);
  return send_json(req, root);
}

/* For text that is already JSON, passed straight through. */
esp_err_t send_raw_json(httpd_req_t *req, const std::string &body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, body.data(), body.size());
  return ESP_OK;
}

esp_err_t reply(httpd_req_t *req, bool ok, const std::string &error) {
  if (!ok)
    httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", error.c_str());
  return send_json(req, r);
}

esp_err_t post_restart(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  httpd_resp_sendstr(req, "{\"ok\":true}");
  restart_soon(500);
  return ESP_OK;
}

/* A restart once the response has gone out (or the client never sees it). */
void restart_soon(uint32_t ms) {
  xTaskCreate(
      [](void *arg) {
        vTaskDelay(pdMS_TO_TICKS(reinterpret_cast<uintptr_t>(arg)));
        esp_restart();
      },
      "restart", 2048, reinterpret_cast<void *>(static_cast<uintptr_t>(ms)), 5, nullptr);
}

/* Long uploads (a firmware, a display firmware) run in a task of their own
   (an async request), so the server keeps answering the page meanwhile -- its
   one task would otherwise be held for the whole transfer, minutes for a
   display firmware. One upload at a time: the second is told so. */
std::atomic<bool> g_uploading{false};

// The upload task's stack, set aside for good: one upload at a time, and a
// heap in pieces (TLS, the event stream) may have no 10 KB block left.
constexpr uint32_t kUploadStack = 10240;

StackType_t g_upload_stack[kUploadStack];

StaticTask_t g_upload_tcb;

TaskHandle_t g_upload_task = nullptr;

/* Whether the last upload's task is gone, so its stack can be used again (it
   clears g_uploading just before it deletes itself). */
bool upload_task_gone() {
  for (int i = 0; i < 20 && g_upload_task && eTaskGetState(g_upload_task) != eDeleted; i++)
    vTaskDelay(pdMS_TO_TICKS(10));
  return !g_upload_task || eTaskGetState(g_upload_task) == eDeleted;
}

template <esp_err_t (*Handler)(httpd_req_t *)>
esp_err_t in_own_task(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (g_uploading.exchange(true)) {
    httpd_resp_set_status(req, "409 Conflict");
    return send_raw_json(req, "{\"ok\":false,\"error\":\"another upload is running\"}");
  }
  if (!upload_task_gone()) {
    g_uploading = false;
    httpd_resp_set_status(req, "409 Conflict");
    return send_raw_json(req, "{\"ok\":false,\"error\":\"another upload is running\"}");
  }
  httpd_req_t *copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    g_uploading = false;
    return httpd_resp_send_500(req);
  }
  g_upload_task = xTaskCreateStatic(
          [](void *arg) {
            auto *r = static_cast<httpd_req_t *>(arg);
            Handler(r);
            httpd_req_async_handler_complete(r);
            g_uploading = false;
            vTaskDelete(nullptr);
          },
          "upload", kUploadStack, copy, 5, g_upload_stack, &g_upload_tcb);
  return ESP_OK;
}

/* Anything we do not serve: to the setup page (a mistyped or old address).

   Except under /api/, where a redirect to an HTML page is a confusing answer to
   a mistyped endpoint. Something expecting JSON should be told it was wrong, in
   JSON. */
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t) {
  if (std::strncmp(req->uri, "/api/", 5) == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"no such endpoint\"}");
    return ESP_OK;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

}  // namespace detail

using namespace detail;


/* The panel CA's certificate (PEM), for a browser or system to trust: public, no login. */
esp_err_t get_tls_ca(httpd_req_t *req) {
  const std::string pem = tls::ca_pem();
  if (pem.empty()) {
    httpd_resp_set_status(req, "404 Not Found");
    return send_raw_json(req, "{\"error\":\"no CA yet\"}");
  }
  httpd_resp_set_type(req, "application/x-pem-file");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ohpanel-ca.crt\"");
  return httpd_resp_send(req, pem.data(), pem.size());
}

/* What the CA and the server certificate are, for the settings page. */
esp_err_t get_tls(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const tls::Info i = tls::info();
  cJSON *r = cJSON_CreateObject();
  cJSON *ca = cJSON_AddObjectToObject(r, "ca");
  cJSON_AddStringToObject(ca, "subject", i.ca_subject.c_str());
  cJSON_AddStringToObject(ca, "fingerprint", i.ca_fingerprint.c_str());
  cJSON_AddStringToObject(ca, "valid_to", i.ca_valid_to.c_str());
  cJSON_AddBoolToObject(ca, "own", i.ca_own);   // uploaded (POST /tls/ca), not made by the panel
  cJSON *srv = cJSON_AddObjectToObject(r, "server");
  cJSON_AddStringToObject(srv, "fingerprint", i.server_fingerprint.c_str());
  cJSON_AddStringToObject(srv, "valid_to", i.server_valid_to.c_str());
  cJSON *names = cJSON_AddArrayToObject(srv, "names");
  for (const auto &n : i.server_names)
    cJSON_AddItemToArray(names, cJSON_CreateString(n.c_str()));
  cJSON_AddBoolToObject(r, "running", g_tls_server != nullptr);
  cJSON_AddBoolToObject(r, "force", tls::force_https());
  cJSON *hosts = cJSON_AddArrayToObject(r, "hosts");
  for (const auto &h : tls::extra_hosts())
    cJSON_AddItemToArray(hosts, cJSON_CreateString(h.c_str()));
  return send_json(req, r);
}

/* {"force": bool}: "Always use https"; {"renew": true}: new keys -- a new CA, which every browser must trust anew;
   {"hosts": [...]}: more names the CA may vouch for and the certificate is issued for (a new CA certificate). */
esp_err_t post_tls(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, 2048)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"bad body\"}");
  }
  cJSON *root = cJSON_Parse(body.c_str());
  bool ok = root != nullptr, remake = false;
  std::string error;
  if (const cJSON *f = cJSON_GetObjectItem(root, "force"); cJSON_IsBool(f))
    ok = ok && tls::set_force_https(cJSON_IsTrue(f));
  if (const cJSON *hl = cJSON_GetObjectItem(root, "hosts"); cJSON_IsArray(hl)) {
    std::vector<std::string> hosts;
    for (const cJSON *h = hl->child; h; h = h->next)
      if (cJSON_IsString(h) && *h->valuestring)
        hosts.emplace_back(h->valuestring);
    ok = ok && tls::set_extra_hosts(hosts, &error);
    remake = true;
  }
  if (ok && cJSON_IsTrue(cJSON_GetObjectItem(root, "renew"))) {
    ok = tls::renew();
    remake = true;
  }
  cJSON_Delete(root);
  if (ok && remake)
    addresses_changed();   // made anew in the background, the https server started with them
  if (!ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", error.empty() ? "could not save" : error.c_str());
    return send_json(req, e);
  }
  return send_raw_json(req, "{\"ok\":true}");
}

/* {"cert": PEM, "key": PEM}: an own CA instead of the panel's (one CA for every panel of a house, trusted once).
   Only in: neither this nor a backup hands its key out again. */
esp_err_t post_tls_ca(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, 10240)) {   // a certificate and a key, a few KB of PEM
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"bad body\"}");
  }
  cJSON *root = cJSON_Parse(body.c_str());
  const char *cert = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cert"));
  const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(root, "key"));
  std::string error = "needs \"cert\" and \"key\"";
  const bool ok = cert && key && tls::set_ca(cert, key, &error);
  cJSON_Delete(root);
  if (!ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", error.c_str());
    return send_json(req, e);
  }
  addresses_changed();   // the server certificate issued by it in the background, the https server restarted
  return send_raw_json(req, "{\"ok\":true}");
}

/* Every route, on the http server and on the https one alike. */
void add_routes(httpd_handle_t server) {
  /* Designated initialisers on purpose: httpd_uri_t has grown fields across
     IDF versions, and positional ones break silently when it does. */
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = get_index},
      {.uri = "/favicon.ico", .method = HTTP_GET, .handler = get_favicon},
      {.uri = "/api/v1/info", .method = HTTP_GET, .handler = get_info},
      {.uri = "/api/v1/config/wifi", .method = HTTP_POST, .handler = post_wifi},
      {.uri = "/api/v1/config/ip", .method = HTTP_POST, .handler = post_ip},
      {.uri = "/api/v1/config/auth", .method = HTTP_POST, .handler = post_auth},
      {.uri = "/api/v1/config/device", .method = HTTP_POST, .handler = post_device},
      {.uri = "/api/v1/config/clock", .method = HTTP_POST, .handler = post_clock},
      {.uri = "/api/v1/config/sensor", .method = HTTP_POST, .handler = post_sensor},
      {.uri = "/api/v1/config/openhab", .method = HTTP_GET, .handler = get_openhab},
      {.uri = "/api/v1/config/openhab", .method = HTTP_POST, .handler = post_openhab},
      {.uri = "/api/v1/openhab/test", .method = HTTP_POST, .handler = post_openhab_test},
      {.uri = "/api/v1/openhab/discover", .method = HTTP_POST, .handler = post_openhab_discover},
      {.uri = "/api/v1/openhab/scan", .method = HTTP_GET, .handler = get_openhab_scan},
      {.uri = "/api/v1/openhab/states", .method = HTTP_GET, .handler = get_openhab_states},
      {.uri = "/api/v1/openhab/items", .method = HTTP_GET, .handler = get_openhab_items},
      {.uri = "/api/v1/openhab/options", .method = HTTP_GET, .handler = get_openhab_options},
      {.uri = "/api/v1/qr", .method = HTTP_POST, .handler = post_qr},
      {.uri = "/api/v1/display/tft", .method = HTTP_POST, .handler = in_own_task<post_display_tft>},
      {.uri = "/api/v1/assets", .method = HTTP_GET, .handler = get_assets},
      {.uri = "/api/v1/layout", .method = HTTP_GET, .handler = get_layout},
      {.uri = "/api/v1/restart", .method = HTTP_POST, .handler = post_restart},
      {.uri = "/api/v1/backup", .method = HTTP_GET, .handler = get_backup},
      {.uri = "/api/v1/backup", .method = HTTP_POST, .handler = post_backup},
      {.uri = "/api/v1/reset", .method = HTTP_POST, .handler = post_reset},
      {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = in_own_task<post_ota>},
      {.uri = "/api/v1/ota/status", .method = HTTP_GET, .handler = get_ota_status},
      {.uri = "/api/v1/transfer", .method = HTTP_GET, .handler = get_transfer},
      {.uri = "/api/v1/log", .method = HTTP_GET, .handler = get_log},
      {.uri = "/api/v1/display/command", .method = HTTP_POST, .handler = post_display_command},
      {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = get_scan},
      {.uri = "/api/v1/panel", .method = HTTP_GET, .handler = get_panel},
      {.uri = "/api/v1/panel", .method = HTTP_PUT, .handler = put_panel},
      {.uri = "/api/v1/panel", .method = HTTP_DELETE, .handler = delete_panel},
      {.uri = "/api/v1/health", .method = HTTP_GET, .handler = get_health},
      {.uri = "/api/v1/coredump", .method = HTTP_GET, .handler = get_coredump},
      {.uri = "/api/v1/coredump", .method = HTTP_DELETE, .handler = delete_coredump},
      {.uri = "/api/v1/tls", .method = HTTP_GET, .handler = get_tls},
      {.uri = "/api/v1/tls", .method = HTTP_POST, .handler = post_tls},
      {.uri = "/api/v1/tls/ca.crt", .method = HTTP_GET, .handler = get_tls_ca},
      {.uri = "/api/v1/tls/ca", .method = HTTP_POST, .handler = post_tls_ca},
  };
  for (const auto &r : routes)
    httpd_register_uri_handler(server, &r);

  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}

/* ---- https ----
   The same interface on port 443, with the certificate of the panel's own CA
   (Tls.h) for the names it goes by now: NAME.local, its address in the home
   network, the setup network's 192.168.4.1. When those change the
   certificate is issued anew and the server started again with it -- in a
   task of its own, as making keys and signing take a moment. */
std::string g_tls_chain, g_tls_key;   // what the running https server holds (it keeps pointers to them)
TaskHandle_t g_tls_task = nullptr;

tls::Names tls_names() {
  tls::Names n;
  std::string host = appcfg::device().name;
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  n.dns.push_back(host + ".local");
  const auto v4 = [&](const std::string &ip) {
    unsigned a, b, c, d;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && (a | b | c | d) != 0)
      n.ips.push_back({static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d)});
  };
  v4(appnet::ip());
  if (n.ips.empty() || n.ips[0] != std::vector<uint8_t>{192, 168, 4, 1})
    v4("192.168.4.1");   // the setup network's
  /* IPv6: a unique local address (the CA covers fc00::/7), a global one only in a prefix the user added (Tls.h);
     link-local ones not -- a browser does not open those without the interface's zone */
  for (const appnet::Address6 &a : appnet::ipv6())
    if (std::strcmp(a.kind, "unique-local") == 0 || (std::strcmp(a.kind, "global") == 0 && tls::covered(a.bytes, 16)))
      n.ips.emplace_back(a.bytes, a.bytes + 16);
  return n;
}

void start_tls() {
  std::string chain, key;
  bool changed = false;
  if (!tls::ensure(tls_names(), chain, key, &changed)) {
    ESP_LOGE(TAG, "No certificate: no https");
    return;
  }
  if (g_tls_server && !changed)
    return;
  if (g_tls_server) {
    httpd_ssl_stop(g_tls_server);
    g_tls_server = nullptr;
  }
  g_tls_chain = chain;
  g_tls_key = key;
  httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
  cfg.httpd.uri_match_fn = [](const char *tpl, const char *uri, size_t len) -> bool {
    g_served = true;
    return std::strlen(tpl) == len && std::strncmp(tpl, uri, len) == 0;
  };
  cfg.httpd.max_open_sockets = 3;   // a TLS session is heavy; the http server has its seven
  cfg.httpd.max_uri_handlers = 48;
  cfg.httpd.lru_purge_enable = true;
  cfg.session_tickets = true;   // a browser coming back resumes its session: no full handshake each time
  cfg.httpd.stack_size = 12288;     // a handshake, and the handlers the http server runs
  cfg.httpd.recv_wait_timeout = 20;
  cfg.httpd.send_wait_timeout = 20;
  cfg.servercert = reinterpret_cast<const uint8_t *>(g_tls_chain.c_str());
  cfg.servercert_len = g_tls_chain.size() + 1;
  cfg.prvtkey_pem = reinterpret_cast<const uint8_t *>(g_tls_key.c_str());
  cfg.prvtkey_len = g_tls_key.size() + 1;
  if (httpd_ssl_start(&g_tls_server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "https server failed to start");
    g_tls_server = nullptr;
    return;
  }
  add_routes(g_tls_server);
  ESP_LOGI(TAG, "HTTPS server up");
}

void tls_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    start_tls();
  }
}

void addresses_changed() {
  if (g_tls_task)
    xTaskNotifyGive(g_tls_task);
}

bool begin(PanelApi panel, DiagnosticsApi diagnostics, DisplayApi display, OtaHook ota_hook) {
  g_panel = std::move(panel);
  g_diag = std::move(diagnostics);
  g_display = std::move(display);
  g_ota_hook = std::move(ota_hook);

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  /* Every request passes through the matcher, which makes it the one place
     that sees them all. It records that somebody reached us, which is what
     lets a freshly flashed image prove itself while it is still serving its
     setup network -- see appweb::has_served(). */
  cfg.uri_match_fn = [](const char *tpl, const char *uri, size_t len) -> bool {
    g_served = true;
    /* Exactly what the server does with no matcher of its own -- this hook is
       here to observe, not to change which handler runs. */
    return std::strlen(tpl) == len && std::strncmp(tpl, uri, len) == 0;
  };
  cfg.max_open_sockets = kMaxSockets;
  cfg.max_uri_handlers = 48;
  cfg.lru_purge_enable = true;
  /* The openHAB connection test runs an HTTP(S) client inside a handler, and a
     TLS handshake needs several kilobytes of stack on its own. */
  cfg.stack_size = 10240;
  /* Firmware uploads arrive in one long request; the default receive timeout is
     too short for a slow uplink. */
  cfg.recv_wait_timeout = 20;
  cfg.send_wait_timeout = 20;

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return false;
  }

  add_routes(g_server);

  // https: the certificate made (the first time: the CA too) and the server started, in the background
  if (xTaskCreate(tls_task, "tls", 8192, nullptr, 3, &g_tls_task) == pdPASS)
    addresses_changed();

  ESP_LOGI(TAG, "HTTP server up, auth %s", auth_required() ? "on" : "off");
  return true;
}

bool has_served() { return g_served.load(); }

}  // namespace appweb
