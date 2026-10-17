/*
   What the web server's files share (Web.cpp and the Web*.cpp of its route groups): the servers and the
   application's hooks, the login check, request and reply helpers, the transfer shown on every page. Private
   to the component.
*/

#pragma once

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
#include <new>
#include <string>
#include <vector>

namespace appweb {
namespace detail {

inline constexpr const char *TAG = "appweb";

/* How many receive timeouts in a row a request may sit through before it is
   given up. The server has a single worker, so a phone that goes to sleep in
   the middle of an upload would otherwise block every other request for good.
   With the 20 s socket timeout this is a minute of silence. */
inline constexpr int kMaxRecvTimeouts = 3;

enum : int { kTxFirmware = 1, kTxDisplay = 2 };   // Transfer::kind (kTransferKinds in Web.cpp)

/* The transfer running now -- a firmware update, a display firmware -- for every page to show its progress
   (GET /api/v1/transfer, also in /api/v1/info). Written by the transfer's
   task, read by handlers: plain words; the error only changes while nothing
   runs. */
struct Transfer {
  std::atomic<int> kind{0};  // kTransferKinds
  std::atomic<uint32_t> done{0}, total{0};
  std::atomic<int64_t> started_us{0}, ended_us{0};
  std::atomic<bool> running{false}, ok{false};
  std::string error;
};

/* Streams the request body to `write` (data, length, error) -- a firmware image, a display firmware -- with bounded
   patience: kMaxRecvTimeouts quiet reads in a row and it gives up. A breath every kBreathEvery bytes: with the data
   coming fast, receiving and writing the flash (where the other core waits too) leave the lower tasks -- the main
   loop, the idle task the watchdog watches -- no turn for the whole upload; the watchdog then restarted the panel
   during the image check (seen with a strong signal). */
template <typename Write>
bool stream_body(httpd_req_t *req, Write write, std::string *error, size_t already = 0) {
  constexpr size_t kBreathEvery = 32 * 1024;
  static char buf[2048];
  size_t left = req->content_len - already;  // `already` read by the caller
  size_t since_breath = 0;
  int timeouts = 0;
  while (left > 0) {
    if (since_breath >= kBreathEvery) {
      since_breath = 0;
      vTaskDelay(1);
    }
    const int got = httpd_req_recv(req, buf, std::min(sizeof(buf), left));
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > kMaxRecvTimeouts) {
        *error = "upload stalled";
        return false;
      }
      continue;
    }
    if (got <= 0) {
      *error = "upload interrupted";
      return false;
    }
    timeouts = 0;
    since_breath += static_cast<size_t>(got);
    if (!write(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(got), error))
      return false;
    left -= got;
  }
  return true;
}

// the application's hooks and the transfer, for the route files (the rest of the shared state is Web.cpp's)
extern PanelApi g_panel;
extern DiagnosticsApi g_diag;
extern DisplayApi g_display;
extern Transfer g_tx;

// the handlers and helpers, by file: Web.cpp, WebSystem.cpp, WebSettings.cpp, WebOpenhab.cpp, WebDisplay.cpp
void indicator_hook(bool active);
bool auth_required();
void hash_header(const std::string &header, uint8_t out[32]);
void forget_auth_cache();
bool check_auth(httpd_req_t *req);
esp_err_t deny(httpd_req_t *req);
esp_err_t send_json(httpd_req_t *req, cJSON *root);
bool read_body(httpd_req_t *req, std::string &out, size_t limit = 1024);
std::string query_param(httpd_req_t *req, const char *key);
void tx_begin(int kind, size_t total);
void tx_progress(size_t done);
void tx_end(bool ok, const std::string &error = "");
void add_transfer(cJSON *parent);
esp_err_t get_log(httpd_req_t *req);
esp_err_t get_transfer(httpd_req_t *req);
esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t handle, const char *reason);
esp_err_t post_ota(httpd_req_t *req);
esp_err_t get_ota_status(httpd_req_t *req);
esp_err_t get_favicon(httpd_req_t *req);
esp_err_t send_to_https(httpd_req_t *req);
bool not_modified(httpd_req_t *req, const char *etag);
esp_err_t send_gzipped(httpd_req_t *req, const unsigned char *data, unsigned int len, const char *type,
                       char (&etag)[12]);
esp_err_t get_index(httpd_req_t *req);
esp_err_t get_layout(httpd_req_t *req);
esp_err_t get_info(httpd_req_t *req);
esp_err_t get_scan(httpd_req_t *req);
esp_err_t post_wifi(httpd_req_t *req);
esp_err_t post_auth(httpd_req_t *req);
bool valid_device_name(const std::string &n);
esp_err_t post_device(httpd_req_t *req);
esp_err_t post_reset(httpd_req_t *req);
esp_err_t send_raw_json(httpd_req_t *req, const std::string &body);
esp_err_t get_panel(httpd_req_t *req);
esp_err_t put_panel(httpd_req_t *req);
esp_err_t delete_panel(httpd_req_t *req);
esp_err_t get_health(httpd_req_t *req);
esp_err_t get_coredump(httpd_req_t *req);
esp_err_t delete_coredump(httpd_req_t *req);
const char *tls_name(appcfg::Tls t);
bool read_ca(const std::string &pem, std::string &summary, std::string &error);
esp_err_t get_openhab(httpd_req_t *req);
bool openhab_from_body(httpd_req_t *req, appcfg::OpenHab &o, std::string &error);
bool openhab_from_json(const cJSON *root, appcfg::OpenHab &o, std::string &error);
esp_err_t post_openhab(httpd_req_t *req);
esp_err_t get_openhab_items(httpd_req_t *req);
esp_err_t get_openhab_options(httpd_req_t *req);
esp_err_t post_qr(httpd_req_t *req);
int upload_baud(httpd_req_t *req);
esp_err_t reply(httpd_req_t *req, bool ok, const std::string &error);
bool valid_host(const std::string &h);
bool valid_tz(const std::string &v);
bool sensor_from_json(const cJSON *o, appcfg::Sensor &s, std::string &error);
bool clock_from_json(const cJSON *o, appcfg::Clock &c, std::string &error);
esp_err_t post_sensor(httpd_req_t *req);
esp_err_t post_clock(httpd_req_t *req);
bool ip_from_json(const cJSON *o, appcfg::Ip &ip, std::string &error);
void add_ip_json(cJSON *o, const appcfg::Ip &ip);
esp_err_t post_ip(httpd_req_t *req);
bool valid_wifi_password(const std::string &pw);
esp_err_t get_backup(httpd_req_t *req);
esp_err_t post_backup(httpd_req_t *req);
esp_err_t post_display_tft(httpd_req_t *req);
esp_err_t post_display_command(httpd_req_t *req);
esp_err_t get_assets(httpd_req_t *req);
esp_err_t get_openhab_states(httpd_req_t *req);
esp_err_t post_openhab_discover(httpd_req_t *req);
void add_discovery(cJSON *r, const std::string &host, const appoh::Discovery &d);
esp_err_t get_openhab_scan(httpd_req_t *req);
esp_err_t post_openhab_test(httpd_req_t *req);
esp_err_t post_restart(httpd_req_t *req);
void restart_soon(uint32_t ms);
bool upload_task_gone();
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t);

}  // namespace detail
}  // namespace appweb
