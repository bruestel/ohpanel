/*
   The settings routes: WiFi and IP, the web password, device, clock and sensor, the panel configuration.
*/

#include "WebDetail.h"

namespace appweb {
namespace detail {

/* Blocks the server for the few seconds a scan takes, which is acceptable for
   a request that only the setup page makes, on demand. */
esp_err_t get_scan(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::vector<appnet::Network> found;
  if (!appnet::scan(found)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"scan not possible right now, try again\"}");
    return ESP_OK;
  }

  cJSON *list = cJSON_CreateArray();
  for (const auto &n : found) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", n.ssid.c_str());
    cJSON_AddNumberToObject(item, "rssi", n.rssi);
    cJSON_AddBoolToObject(item, "secured", n.secured);
    cJSON_AddStringToObject(item, "security", n.security);
    cJSON_AddItemToArray(list, item);
  }
  return send_json(req, list);
}

esp_err_t post_wifi(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : nullptr;
  cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : nullptr;
  if (!cJSON_IsString(ssid)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"ssid required\"}");
    return ESP_OK;
  }

  /* Open (no password) or WPA2/WPA3: a passphrase of 8 to 63 characters, or
     the key itself as 64 hex digits -- shorter ones are WEP keys, which the
     panel does not join. */
  const std::string pw = cJSON_IsString(pass) ? pass->valuestring : "";
  if (!valid_wifi_password(pw)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"a WPA2/WPA3 password has 8 to 63 characters (or none, for an open network)\"}");
    return ESP_OK;
  }
  // "wpa3": of a WPA2/WPA3 network WPA3 only; otherwise ("auto") the best both offer
  const cJSON *sec = cJSON_GetObjectItem(root, "security");
  const bool wpa3_only = cJSON_IsString(sec) && std::strcmp(sec->valuestring, "wpa3") == 0;
  if (wpa3_only && pw.empty()) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"WPA3 needs a password\"}");
    return ESP_OK;
  }
  /* The address it gets there: as given ("ip", see ip_from_json); without one, another network starts with DHCP --
     a static address of the old one is hardly one of the new. */
  appcfg::Ip ip = appcfg::ip();
  std::string ip_error;
  const cJSON *ipj = cJSON_GetObjectItem(root, "ip");
  if (cJSON_IsObject(ipj)) {
    if (!ip_from_json(ipj, ip, ip_error)) {
      cJSON_Delete(root);
      return reply(req, false, "IP address: " + ip_error);
    }
  } else if (appcfg::wifi().ssid != ssid->valuestring) {
    ip.fixed = false;
  }
  const bool ok = appcfg::set_wifi(ssid->valuestring, pw, wpa3_only) && appcfg::set_ip(ip);
  cJSON_Delete(root);

  if (!ok) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

  /* New credentials are applied by restarting, not by reconnecting in place.
     Switching from the setup network to a station while both are up puts the
     one radio in an impossible position: the access point is on a fixed
     channel and the station has to follow the router's, which on a house with
     two access points on different channels is a different one. The device
     then reports itself connected -- correctly -- while barely passing a
     packet, and the phone that was reading the address loses the setup network
     from under it, because that moved channel too.

     A restart costs about five seconds and removes the whole situation: on the
     way up there is no access point to accommodate, and the station connects
     to whatever channel it likes. Measured stable over 150 consecutive probes
     after a restart, against an unreachable device after an in-place switch.

     After the response has gone out, or the client never sees it. */
  restart_soon(500);
  return ESP_OK;
}

esp_err_t post_auth(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    return ESP_OK;
  }

  cJSON *pw = cJSON_GetObjectItem(root, "password");
  cJSON *user = cJSON_GetObjectItem(root, "user");
  cJSON *enabled = cJSON_GetObjectItem(root, "enabled");

  const char *error = nullptr;
  if (cJSON_IsString(pw)) {
    if (!appcfg::set_password(cJSON_IsString(user) ? user->valuestring : "admin", pw->valuestring))
      error = "could not store password";
  } else if (cJSON_IsString(user)) {  // renamed, the password kept
    if (!appcfg::set_auth_user(user->valuestring))
      error = "could not store the user";
  }

  if (!error && cJSON_IsBool(enabled))
    if (!appcfg::set_auth_enabled(cJSON_IsTrue(enabled)))
      error = "cannot enable protection without a password";

  cJSON_Delete(root);

  if (error) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error);
    return send_json(req, r);
  }

  /* The old credentials must stop working now, not in five minutes. */
  forget_auth_cache();

  appcfg::Auth a = appcfg::auth();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "enabled", a.enabled);
  cJSON_AddBoolToObject(r, "has_password", a.has_password());
  return send_json(req, r);
}

/* The name doubles as the DHCP hostname, the mDNS name and the access-point
   SSID, so it has to be a valid DNS label -- not merely non-empty. Rejecting
   here beats letting the router or a phone quietly mangle it later. */
bool valid_device_name(const std::string &n) {
  if (n.empty() || n.size() > 32)
    return false;
  if (n.front() == '-' || n.back() == '-')
    return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return true;
}

esp_err_t post_device(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *name = root ? cJSON_GetObjectItem(root, "name") : nullptr;
  if (!cJSON_IsString(name)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"name required\"}");
    return ESP_OK;
  }

  /* Lowercase rather than reject: hostnames are case-insensitive anyway, and
     someone typing a capital should not be met with an error. */
  std::string n = name->valuestring;
  for (char &c : n)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  cJSON_Delete(root);

  /* An empty name reverts to the MAC-derived default. Without this there is no
     way back to it short of a factory reset. */
  if (n.empty()) {
    appcfg::set_device_name("");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "name", appcfg::default_device_name().c_str());
    cJSON_AddBoolToObject(r, "restart_required", true);
    return send_json(req, r);
  }

  if (!valid_device_name(n)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req,
                       "{\"error\":\"1-32 characters, a-z 0-9 and dashes, not starting or ending with a dash\"}");
    return ESP_OK;
  }

  if (!appcfg::set_device_name(n)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddStringToObject(r, "name", n.c_str());
  /* Hostname and mDNS are bound at interface setup, so the new name only takes
     effect on the next boot. Say so rather than let the user wonder. */
  cJSON_AddBoolToObject(r, "restart_required", true);
  return send_json(req, r);
}

/* The panel configuration: one JSON document, read, replaced and reset.

   Saving goes through the application's validator first, so a rejected
   document changes nothing at all. ?download=1 serves it as a file, which is
   what the backup button fetches; uploading a backup is a PUT like any other
   save, done by the browser after reading the file. */
esp_err_t get_panel(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const std::string json = g_panel.load ? g_panel.load() : std::string("{}");
  if (query_param(req, "download") == "1") {
    httpd_resp_set_type(req, "application/json");
    /* The device name is a validated DNS label (see valid_device_name), so it
       cannot break out of the header. */
    const std::string disp = "attachment; filename=\"" + appcfg::device().name + ".json\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    httpd_resp_send(req, json.data(), json.size());
    return ESP_OK;
  }
  return send_raw_json(req, json);
}

esp_err_t put_panel(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body, kMaxPanelBytes)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"missing or oversized body\"}");
  }

  std::string err;
  const bool ok = g_panel.save && g_panel.save(body, &err);
  if (!ok)
    httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", err.empty() ? "could not save" : err.c_str());
  return send_json(req, r);
}

esp_err_t delete_panel(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool ok = g_panel.reset && g_panel.reset();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  return send_json(req, r);
}

/* A host name or address for the time server: letters, digits, dots, dashes
   (and colons, for IPv6). Empty is valid: the router's server, then the pool. */
bool valid_host(const std::string &h) {
  if (h.size() > 63)
    return false;
  for (char c : h)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != ':')
      return false;
  return true;
}

/* What the C library reads from TZ, and an IANA name: printable, no spaces. */
bool valid_tz(const std::string &v) {
  if (v.empty() || v.size() > 47)
    return false;
  for (char c : v)
    if (c <= ' ' || c > '~')
      return false;
  return true;
}

/* The temperature sensor: {"offset_tenths", "item"}, each optional. An empty
   item stops reporting to openHAB. */
/* {"offset_tenths", "item"}, each optional, onto `s`; false with `error` if one makes no sense. Shared with a
   backup's restore. */
bool sensor_from_json(const cJSON *o, appcfg::Sensor &s, std::string &error) {
  if (const cJSON *off = cJSON_GetObjectItem(o, "offset_tenths"); cJSON_IsNumber(off)) {
    s.offset_tenths = off->valueint;
    if (off->valueint < -200 || off->valueint > 200)
      error = "sensor offset: -200..200 tenths of a degree";
  }
  if (const cJSON *item = cJSON_GetObjectItem(o, "item"); cJSON_IsString(item)) {
    s.item = item->valuestring;
    for (char c : s.item)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        error = "sensor item: letters, digits and _";
  }
  return error.empty();
}

esp_err_t post_sensor(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  cJSON *root = read_body(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
  if (!root)
    return reply(req, false, "bad body");
  appcfg::Sensor s = appcfg::sensor();
  std::string error;
  const bool ok = sensor_from_json(root, s, error);
  cJSON_Delete(root);
  if (!ok)
    return reply(req, false, error);
  return reply(req, appcfg::set_sensor(s), "store failed");
}

/* Time server and timezone, {"ntp_server", "tz_name", "tz_posix"}, each
   optional but the two timezone fields together. Take effect at once. */
bool clock_from_json(const cJSON *o, appcfg::Clock &c, std::string &error) {
  const cJSON *ntp = cJSON_GetObjectItem(o, "ntp_server");
  const cJSON *tzn = cJSON_GetObjectItem(o, "tz_name");
  const cJSON *tzp = cJSON_GetObjectItem(o, "tz_posix");
  if (cJSON_IsString(tzn) != cJSON_IsString(tzp)) {
    error = "tz_name and tz_posix go together";
    return false;
  }
  if (cJSON_IsString(ntp))
    c.server = ntp->valuestring;
  if (cJSON_IsString(tzn)) {
    c.tz_name = tzn->valuestring;
    c.tz_posix = tzp->valuestring;
  }
  if (!valid_host(c.server) || !valid_tz(c.tz_name) || !valid_tz(c.tz_posix))
    error = "invalid time server or timezone";
  return error.empty();
}

esp_err_t post_clock(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  cJSON *root = read_body(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
  if (!root)
    return reply(req, false, "bad body");
  appcfg::Clock c = appcfg::clock();
  std::string error;
  const bool ok = clock_from_json(root, c, error);
  cJSON_Delete(root);
  if (!ok)
    return reply(req, false, error);
  if (!appcfg::set_clock(c))
    return reply(req, false, "store failed");
  appnet::apply_clock();
  return reply(req, true, "");
}

/* The station's address: {"mode": "dhcp"} or {"mode": "static", "address", "prefix", "gateway", "dns1", "dns2"}
   (the DNS servers optional: the gateway then). False with `error` if it makes no sense (appnet::valid_ip). */
bool ip_from_json(const cJSON *o, appcfg::Ip &ip, std::string &error) {
  const auto str = [&](const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
  };
  const cJSON *mode = cJSON_GetObjectItem(o, "mode");
  if (!cJSON_IsString(mode) || (std::strcmp(mode->valuestring, "dhcp") && std::strcmp(mode->valuestring, "static"))) {
    error = "mode: dhcp or static";
    return false;
  }
  ip.fixed = std::strcmp(mode->valuestring, "static") == 0;
  // what a static one was stays stored with DHCP too: switching back finds it filled in
  if (cJSON_GetObjectItem(o, "address")) ip.address = str("address");
  if (cJSON_GetObjectItem(o, "gateway")) ip.gateway = str("gateway");
  if (cJSON_GetObjectItem(o, "dns1")) ip.dns1 = str("dns1");
  if (cJSON_GetObjectItem(o, "dns2")) ip.dns2 = str("dns2");
  if (const cJSON *p = cJSON_GetObjectItem(o, "prefix"); cJSON_IsNumber(p)) ip.prefix = p->valueint;
  if (const cJSON *v6 = cJSON_GetObjectItem(o, "ipv6"); cJSON_IsBool(v6)) ip.ipv6 = cJSON_IsTrue(v6);
  return !ip.fixed || appnet::valid_ip(ip, &error);
}

void add_ip_json(cJSON *o, const appcfg::Ip &ip) {
  cJSON_AddStringToObject(o, "mode", ip.fixed ? "static" : "dhcp");
  cJSON_AddStringToObject(o, "address", ip.address.c_str());
  cJSON_AddNumberToObject(o, "prefix", ip.prefix);
  cJSON_AddStringToObject(o, "gateway", ip.gateway.c_str());
  cJSON_AddStringToObject(o, "dns1", ip.dns1.c_str());
  cJSON_AddStringToObject(o, "dns2", ip.dns2.c_str());
  cJSON_AddBoolToObject(o, "ipv6", ip.ipv6);
}

/* DHCP or a fixed address; applied by restarting (the station joins with it on the way up, see post_wifi). */
esp_err_t post_ip(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body))
    return reply(req, false, "bad body");
  cJSON *root = cJSON_Parse(body.c_str());
  appcfg::Ip ip = appcfg::ip();
  std::string error;
  const bool valid = root && ip_from_json(root, ip, error);
  cJSON_Delete(root);
  if (!valid)
    return reply(req, false, error.empty() ? "bad body" : error);
  if (!appcfg::set_ip(ip))
    return reply(req, false, "store failed");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
  restart_soon(500);
  return ESP_OK;
}

// ----------------------------------------------------------------- backup --

/* A WPA2/WPA3 passphrase (8 to 63 characters, or the key as 64 hex digits),
   or none for an open network. */
bool valid_wifi_password(const std::string &pw) {
  const bool hex64 = pw.size() == 64 && std::all_of(pw.begin(), pw.end(), [](char c) {
                       return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                     });
  return pw.empty() || (pw.size() >= 8 && (pw.size() <= 63 || hex64));
}

}  // namespace detail
}  // namespace appweb
