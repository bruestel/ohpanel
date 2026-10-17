/*
   The display routes: a display firmware, a command, the icon set, a QR code.
*/

#include "WebDetail.h"

namespace appweb {
namespace detail {

/* {"text": "...", "ecc": "low|medium|quartile|high"} -- the QR code the QR
   page draws (Engine::draw_qr_code, the same encoder and choices), as
   {"size": n, "modules": "0110..."} row by row, for the web preview. */
esp_err_t post_qr(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, 1024)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"bad body\"}");
  }
  cJSON *root = cJSON_Parse(body.c_str());
  const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
  const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ecc"));
  const std::string ecc = e ? e : "medium";
  const qrcodegen_Ecc level = ecc == "low" ? qrcodegen_Ecc_LOW : ecc == "quartile" ? qrcodegen_Ecc_QUARTILE
                              : ecc == "high" ? qrcodegen_Ecc_HIGH : qrcodegen_Ecc_MEDIUM;
  constexpr int kMaxVersion = 10;   // Engine::draw_qr_code
  std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)), tmp(qr.size());
  bool ok = text && *text && qrcodegen_encodeText(text, tmp.data(), qr.data(), level, qrcodegen_VERSION_MIN, kMaxVersion,
                                                  qrcodegen_Mask_AUTO, true);
  if (!ok && text && *text)
    ok = qrcodegen_encodeText(text, tmp.data(), qr.data(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, kMaxVersion,
                              qrcodegen_Mask_AUTO, false);
  cJSON_Delete(root);
  cJSON *r = cJSON_CreateObject();
  if (ok) {
    const int n = qrcodegen_getSize(qr.data());
    std::string modules;
    modules.reserve(n * n);
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        modules += qrcodegen_getModule(qr.data(), x, y) ? '1' : '0';
    cJSON_AddNumberToObject(r, "size", n);
    cJSON_AddStringToObject(r, "modules", modules.c_str());
  } else {
    cJSON_AddStringToObject(r, "error", text && *text ? "too long for the card" : "no text");
  }
  return send_json(req, r);
}

// ---------------------------------------------------------------- display ----

/* 921600, the design's rate; ?baud=115200 still takes the slow one, for a display that fails at the fast one. */
int upload_baud(httpd_req_t *req) { return query_param(req, "baud") == "115200" ? 115200 : 921600; }

/* The body is a .tft, written straight through to the display. Blocks the
   server for the whole transfer (minutes at 115200); the browser's upload
   progress follows the display, since the socket only drains as fast as the
   display confirms blocks. */
esp_err_t post_display_tft(httpd_req_t *req) {   // in_own_task: the password checked already
  std::string error;
  if (!g_display.tft_begin || req->content_len < 1024)
    return reply(req, false, "not a display firmware");
  /* A display firmware starts 00 01 ?? 4E 32 45 (the third byte is the
     Editor's version); anything else the display would be asked to take in
     and reject only at the end, minutes later -- or worse. */
  uint8_t head[8];
  size_t got = 0;
  for (int timeouts = 0; got < sizeof(head);) {
    const int n = httpd_req_recv(req, reinterpret_cast<char *>(head) + got, sizeof(head) - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= kMaxRecvTimeouts)
      continue;
    if (n <= 0)
      return reply(req, false, "upload interrupted");
    got += n;
  }
  if (head[0] != 0x00 || head[1] != 0x01 || head[3] != 0x4E || head[4] != 0x32 || head[5] != 0x45)
    return reply(req, false, "not a display firmware (.tft)");
  if (!g_display.tft_begin(req->content_len, upload_baud(req), &error))
    return reply(req, false, error);
  tx_begin(kTxDisplay, req->content_len);
  bool ok = g_display.tft_write(head, sizeof(head), &error);
  tx_progress(sizeof(head));
  ok = ok && stream_body(
                 req,
                 [](const uint8_t *d, size_t n, std::string *e) {
                   tx_progress(g_tx.done + n);
                   return g_display.tft_write(d, n, e);
                 },
                 &error, sizeof(head));
  if (ok)
    ok = g_display.tft_end(&error);
  else
    g_display.tft_abort();
  tx_end(ok, error);
  return reply(req, ok, error);
}

/* {"command": "..."} -- one command straight to the display, for trying
   things out (the diagnostics page's console); the display's answers show in
   the log. */
esp_err_t post_display_command(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, 512))
    return reply(req, false, "bad body");
  cJSON *root = cJSON_Parse(body.c_str());
  const cJSON *c = cJSON_GetObjectItem(root, "command");
  const std::string command = cJSON_IsString(c) ? c->valuestring : "";
  const cJSON *log = cJSON_GetObjectItem(root, "log");  // {"log": true}: log every command sent
  if (cJSON_IsBool(log) && g_display.log_commands) {
    g_display.log_commands(cJSON_IsTrue(log));
    cJSON_Delete(root);
    return reply(req, true, "");
  }
  cJSON_Delete(root);
  if (command.empty() || !g_display.command)
    return reply(req, false, "no command");
  g_display.command(command);
  return reply(req, true, "");
}

/* The icon set embedded in the firmware (tools/gen_display_assets.py has the
   format), for the web interface's icons. No login needed, like the page
   itself; the browser revalidates it by its version. */
esp_err_t get_assets(httpd_req_t *req) {
  const AssetsInfo info = g_display.assets_info ? g_display.assets_info() : AssetsInfo{};
  const uint8_t *data = g_display.assets_data ? g_display.assets_data() : nullptr;
  if (!data || !info.size) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"no icon set\"}");
    return ESP_OK;
  }
  char etag[12];
  snprintf(etag, sizeof(etag), "\"%08lx\"", static_cast<unsigned long>(info.version));
  if (not_modified(req, etag))
    return ESP_OK;
  httpd_resp_set_type(req, "application/octet-stream");
  for (size_t sent = 0; sent < info.size;) {  // straight from the flash, in pieces
    const size_t chunk = std::min<size_t>(4096, info.size - sent);
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(data + sent), chunk) != ESP_OK)
      return ESP_FAIL;
    sent += chunk;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace detail
}  // namespace appweb
