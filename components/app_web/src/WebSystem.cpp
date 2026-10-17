/*
   Firmware update, backup and reset, health and the core dump: the maintenance routes.
*/

#include "WebDetail.h"

namespace appweb {
namespace detail {

/* ?since=<n> -- the log from position n on (the text and where to go on), for
   the diagnostics page to follow it. */
esp_err_t get_log(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  uint32_t next = 0;
  const uint32_t since = strtoul(query_param(req, "since").c_str(), nullptr, 10);
  const std::string text = g_diag.read_log ? g_diag.read_log(since, &next) : "";
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "next", next);
  cJSON_AddStringToObject(r, "text", text.c_str());
  return send_json(req, r);
}

esp_err_t get_transfer(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  cJSON *r = cJSON_CreateObject();
  add_transfer(r);
  return send_json(req, r);
}

esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t handle, const char *reason) {
  tx_end(false, reason);
  if (handle)
    esp_ota_abort(handle);
  ESP_LOGE(TAG, "OTA rejected: %s", reason);
  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", reason);
  return send_json(req, r);
}

esp_err_t post_ota(httpd_req_t *req) {   // in_own_task: the password checked already

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!target)
    return ota_fail(req, 0, "no free OTA slot");

  const size_t total = req->content_len;
  if (total < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
    return ota_fail(req, 0, "not a firmware image (too small)");
  if (total > target->size)
    return ota_fail(req, 0, "image larger than the OTA slot -- one for another partition table, which only a "
                            "serial flash brings (docs/flashing.md)");

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, total, &handle);
  if (err != ESP_OK)
    return ota_fail(req, 0, esp_err_to_name(err));

  ESP_LOGW(TAG, "OTA started: %u bytes into %s", static_cast<unsigned>(total), target->label);
  tx_begin(kTxFirmware, total);
  indicator_hook(true);

  size_t written = 0;
  bool checked = false;

  /* The image header, the first segment header and the application descriptor,
     which is as far as we have to read to know whose firmware this is. */
  constexpr size_t kDescAt = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  constexpr size_t kHeaderNeeded = kDescAt + sizeof(esp_app_desc_t);
  std::vector<uint8_t> head;
  head.reserve(kHeaderNeeded);
  std::string error;
  const bool ok = stream_body(req, [&](const uint8_t *data, size_t len, std::string *why) {
    /* Nothing reaches the flash before this passes. The magic byte catches
       anything that is not an ESP image at all; the project name catches
       firmware for a different device, which would otherwise brick this one in
       a way only a cable can undo.

       The header is collected across however many reads it takes rather than
       expected in the first one. A slow uplink delivers a short first chunk,
       and this used to take that as permission to skip the project name and
       write the rest unchecked. */
    if (!checked) {
      const size_t take = std::min(kHeaderNeeded - head.size(), len);
      head.insert(head.end(), data, data + take);
      data += take;
      len -= take;
      if (!head.empty() && head[0] != ESP_IMAGE_HEADER_MAGIC) {
        *why = "not an ESP firmware image";
        return false;
      }
      if (head.size() < kHeaderNeeded)
        return true;  // keep reading; still nothing written
      const auto *desc = reinterpret_cast<const esp_app_desc_t *>(head.data() + kDescAt);
      const esp_app_desc_t *self = esp_app_get_description();
      if (std::strncmp(desc->project_name, self->project_name, sizeof(desc->project_name)) != 0) {
        *why = "firmware is for a different project";
        return false;
      }
      ESP_LOGI(TAG, "Incoming version: %s", desc->version);
      checked = true;
      if (const esp_err_t e = esp_ota_write(handle, head.data(), head.size()); e != ESP_OK) {
        *why = esp_err_to_name(e);
        return false;
      }
      written += head.size();
      tx_progress(written);
      if (len == 0)
        return true;
    }
    if (const esp_err_t e = esp_ota_write(handle, data, len); e != ESP_OK) {
      *why = esp_err_to_name(e);
      return false;
    }
    written += len;
    tx_progress(written);
    return true;
  }, &error);
  if (!ok) {
    indicator_hook(false);
    return ota_fail(req, handle, error.c_str());
  }

  vTaskDelay(1);   // a breath before the image check, which reads it all (see kBreathEvery)
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : esp_err_to_name(err));
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, esp_err_to_name(err));
  }

  ESP_LOGW(TAG, "OTA complete, rebooting into %s", target->label);
  tx_end(true);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddNumberToObject(r, "written", written);
  send_json(req, r);
  restart_soon(700);
  return ESP_OK;
}

esp_err_t get_ota_status(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  const esp_app_desc_t *self = esp_app_get_description();

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "partition", running->label);
  cJSON_AddStringToObject(r, "version", self->version);
  cJSON_AddStringToObject(r, "built", self->date);
  cJSON_AddBoolToObject(r, "pending_verify", state == ESP_OTA_IMG_PENDING_VERIFY);
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  cJSON_AddNumberToObject(r, "slot_size", next ? next->size : 0);
  return send_json(req, r);
}

esp_err_t post_reset(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Erases every setting and reboots, so the device comes back as if freshly
     flashed. Unlike the button hold -- which only clears what can lock you out
     -- this is a deliberate, confirmable action from a browser, so it can
     afford to be complete. */
  /* The panel layout goes too: "as if freshly flashed" includes it. */
  bool ok = appcfg::factory_reset();
  if (g_panel.reset)
    ok = g_panel.reset() && ok;
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "could not erase all settings");
  send_json(req, r);

  if (ok)
    restart_soon(500);
  return ESP_OK;
}

esp_err_t get_health(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_diag.health_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_diag.health_json());
}

/* Streamed in chunks: a core dump is tens of kilobytes and holding it in memory
   to send it would risk the very condition it was recorded for. */
esp_err_t get_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const size_t total = g_diag.coredump_size ? g_diag.coredump_size() : 0;
  if (!total) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"no core dump stored\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

  char buf[1024];
  size_t sent = 0;
  while (sent < total) {
    const size_t chunk = std::min(sizeof(buf), total - sent);
    if (!g_diag.read_coredump(sent, buf, chunk)) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, buf, chunk) != ESP_OK)
      return ESP_FAIL;
    sent += chunk;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t delete_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool ok = g_diag.erase_coredump && g_diag.erase_coredump();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "nothing to erase, or erase failed");
  return send_json(req, r);
}

/* Everything the panel is set up with, as one file:
   ?panel=1 with the panel configuration, ?secrets=1 with the WiFi password,
   the openHAB token and the web password's hash. Without them a restore
   keeps the ones the panel has. */
esp_err_t get_backup(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool with_panel = query_param(req, "panel") == "1";
  const bool secrets = query_param(req, "secrets") == "1";
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "ohpanel_backup", 1);
  cJSON_AddStringToObject(r, "firmware", esp_app_get_description()->version);
  cJSON *dev = cJSON_AddObjectToObject(r, "device");
  cJSON_AddStringToObject(dev, "name", appcfg::device().name.c_str());
  const appcfg::Wifi w = appcfg::wifi();
  cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
  cJSON_AddStringToObject(wifi, "ssid", w.ssid.c_str());
  cJSON_AddStringToObject(wifi, "security", w.wpa3_only ? "wpa3" : "auto");
  if (secrets)
    cJSON_AddStringToObject(wifi, "password", w.password.c_str());
  add_ip_json(cJSON_AddObjectToObject(wifi, "ip"), appcfg::ip());
  const appcfg::Auth a = appcfg::auth();
  cJSON *auth = cJSON_AddObjectToObject(r, "auth");
  cJSON_AddBoolToObject(auth, "enabled", a.enabled);
  cJSON_AddStringToObject(auth, "user", a.user.c_str());
  if (secrets && a.has_password()) {
    cJSON_AddStringToObject(auth, "salt", a.salt_hex.c_str());
    cJSON_AddStringToObject(auth, "hash", a.hash_hex.c_str());
  }
  const appcfg::OpenHab o = appcfg::openhab();
  cJSON *oh = cJSON_AddObjectToObject(r, "openhab");
  cJSON_AddStringToObject(oh, "url", o.url.c_str());
  cJSON_AddStringToObject(oh, "tls", tls_name(o.tls));
  cJSON_AddStringToObject(oh, "ca", o.ca.c_str());
  cJSON_AddBoolToObject(oh, "check_name", o.check_name);
  if (secrets)
    cJSON_AddStringToObject(oh, "token", o.token.c_str());
  const appcfg::Clock c = appcfg::clock();
  cJSON *clock = cJSON_AddObjectToObject(r, "clock");
  cJSON_AddStringToObject(clock, "ntp_server", c.server.c_str());
  cJSON_AddStringToObject(clock, "tz_name", c.tz_name.c_str());
  cJSON_AddStringToObject(clock, "tz_posix", c.tz_posix.c_str());
  const appcfg::Sensor sn = appcfg::sensor();
  cJSON *sensor = cJSON_AddObjectToObject(r, "sensor");
  cJSON_AddNumberToObject(sensor, "offset_tenths", sn.offset_tenths);
  cJSON_AddStringToObject(sensor, "item", sn.item.c_str());
  cJSON_AddNumberToObject(r, "tx_power_dbm", appcfg::tx_power_dbm());
  // "Always use https" -- not the keys: every panel makes its own CA (Tls.cpp)
  cJSON_AddBoolToObject(cJSON_AddObjectToObject(r, "https"), "force", tls::force_https());
  if (with_panel && g_panel.load) {
    cJSON *panel = cJSON_Parse(g_panel.load().c_str());
    if (panel)
      cJSON_AddItemToObject(r, "panel", panel);
  }
  char *text = cJSON_Print(r);
  cJSON_Delete(r);
  if (!text)
    return reply(req, false, "out of memory");
  httpd_resp_set_type(req, "application/json");
  const std::string disp = "attachment; filename=\"" + appcfg::device().name + "-backup.json\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  httpd_resp_send(req, text, std::strlen(text));
  cJSON_free(text);
  return ESP_OK;
}

/* A backup back in: everything in it is checked first -- nothing changes if
   anything is wrong -- then stored, and the panel restarts (name, WiFi and
   the rest take effect on the way up). What a backup leaves out stays as it
   is: secrets it does not hold, the panel configuration. A WiFi network
   without its password in the backup (made without the secrets; an open
   network has "" there): the same network keeps the stored password; in the
   setup hotspot, where there is no network to lose, it is taken without one
   (an open network joins, another one brings the hotspot back to enter the
   password); a panel on another network keeps that one. */
esp_err_t post_backup(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, kMaxPanelBytes + 16 * 1024))
    return reply(req, false, "missing or oversized body");
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root || !cJSON_IsNumber(cJSON_GetObjectItem(root, "ohpanel_backup"))) {
    cJSON_Delete(root);
    return reply(req, false, "not an OHpanel backup");
  }
  std::vector<std::string> skipped;
  std::string error;
  auto field = [](const cJSON *o, const char *k) { return cJSON_GetObjectItem(o, k); };

  // check everything
  std::string name;
  if (const cJSON *n = field(field(root, "device"), "name"); cJSON_IsString(n)) {
    name = n->valuestring;
    if (!name.empty() && !valid_device_name(name))
      error = "device name: 1-32 characters, a-z 0-9 and dashes";
  }
  const cJSON *wifi = field(root, "wifi");
  const cJSON *ssid = field(wifi, "ssid"), *wpw = field(wifi, "password");
  if (cJSON_IsString(wpw) && !valid_wifi_password(wpw->valuestring))
    error = "WiFi password: 8 to 63 characters, or none";
  appcfg::Ip ip = appcfg::ip();
  const cJSON *ipj = field(wifi, "ip");
  if (cJSON_IsObject(ipj) && error.empty() && !ip_from_json(ipj, ip, error))
    error = "IP address: " + error;
  appcfg::OpenHab oh = appcfg::openhab();
  if (const cJSON *o = field(root, "openhab"); cJSON_IsObject(o) && error.empty())
    if (!openhab_from_json(o, oh, error))
      error = "openHAB: " + error;
  appcfg::Clock clock = appcfg::clock();
  if (const cJSON *c = field(root, "clock"); cJSON_IsObject(c) && error.empty())
    clock_from_json(c, clock, error);
  appcfg::Sensor sensor = appcfg::sensor();
  if (const cJSON *sn = field(root, "sensor"); cJSON_IsObject(sn) && error.empty())
    sensor_from_json(sn, sensor, error);
  const cJSON *auth = field(root, "auth");
  const cJSON *salt = field(auth, "salt"), *hash = field(auth, "hash");
  if (cJSON_IsString(salt) != cJSON_IsString(hash))
    error = "auth: salt and hash go together";
  if (!error.empty()) {
    cJSON_Delete(root);
    return reply(req, false, error);
  }

  // the panel configuration first: its own check may still say no, and nothing has changed yet
  if (const cJSON *panel = field(root, "panel"); cJSON_IsObject(panel)) {
    char *text = cJSON_PrintUnformatted(panel);
    std::string perr;
    const bool ok = text && g_panel.save && g_panel.save(text, &perr);
    cJSON_free(text);
    if (!ok) {
      cJSON_Delete(root);
      return reply(req, false, "panel configuration: " + (perr.empty() ? std::string("could not save") : perr));
    }
  } else {
    skipped.push_back("the panel configuration (not in the backup)");
  }

  // then the settings; one that cannot be stored is reported with the skipped ones
  const auto stored = [&skipped](bool ok, const char *what) {
    if (!ok)
      skipped.push_back(std::string(what) + " (could not be stored)");
  };
  if (cJSON_IsString(field(field(root, "device"), "name")))
    stored(appcfg::set_device_name(name), "the device name");
  if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
    const appcfg::Wifi now = appcfg::wifi();
    const bool wpa3 = cJSON_IsString(field(wifi, "security")) &&
                      std::strcmp(field(wifi, "security")->valuestring, "wpa3") == 0;
    if (cJSON_IsString(wpw)) {
      stored(appcfg::set_wifi(ssid->valuestring, wpw->valuestring, wpa3 && wpw->valuestring[0]), "WiFi");
    } else if (now.ssid == ssid->valuestring) {
      stored(appcfg::set_wifi(now.ssid, now.password, wpa3 && !now.password.empty()), "WiFi");
    } else if (appnet::provisioning()) {
      stored(appcfg::set_wifi(ssid->valuestring, "", false), "WiFi");
      skipped.push_back("the WiFi password (not in the backup): if \"" + std::string(ssid->valuestring) +
                        "\" has one, enter it under Settings > WiFi once the hotspot is back");
    } else {
      skipped.push_back("WiFi \"" + std::string(ssid->valuestring) +
                        "\" (its password is not in the backup; the panel stays on its network)");
    }
  }
  if (cJSON_IsObject(ipj))
    stored(appcfg::set_ip(ip), "the IP address");
  stored(appcfg::set_openhab(oh), "openHAB");
  stored(appcfg::set_clock(clock), "the clock");
  stored(appcfg::set_sensor(sensor), "the sensor");
  if (const cJSON *tx = field(root, "tx_power_dbm"); cJSON_IsNumber(tx) && tx->valueint >= 0 && tx->valueint <= 21)
    stored(appcfg::set_tx_power_dbm(tx->valueint), "the transmit power");
  if (const cJSON *f = field(field(root, "https"), "force"); cJSON_IsBool(f))   // (not while the hotspot serves)
    stored(tls::set_force_https(cJSON_IsTrue(f)), "\"Always use https\"");
  if (cJSON_IsObject(auth)) {
    const cJSON *user = field(auth, "user");
    const std::string u = cJSON_IsString(user) ? user->valuestring : appcfg::auth().user;
    if (cJSON_IsString(salt)) {
      if (!appcfg::set_auth_hash(u, salt->valuestring, hash->valuestring))
        skipped.push_back("the web password (not readable)");
    } else {
      stored(appcfg::set_auth_user(u), "the web user");
    }
    if (const cJSON *en = field(auth, "enabled"); cJSON_IsBool(en))
      if (!appcfg::set_auth_enabled(cJSON_IsTrue(en)))
        skipped.push_back("password protection (no password)");
  }
  cJSON_Delete(root);
  forget_auth_cache();

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "restarting", true);
  cJSON *list = cJSON_AddArrayToObject(r, "skipped");
  for (const auto &s_ : skipped)
    cJSON_AddItemToArray(list, cJSON_CreateString(s_.c_str()));
  send_json(req, r);
  restart_soon(800);
  return ESP_OK;
}

}  // namespace detail
}  // namespace appweb
