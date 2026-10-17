/*
   Persistent configuration in NVS.
*/

#include "appcfg/Config.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>

namespace appcfg {
namespace {

const char *const TAG = "appcfg";

const char *const kNsWifi = "wifi";
const char *const kNsAuth = "auth";
const char *const kNsDev = "device";
const char *const kNsOpenHab = "openhab";

/* Deliberately on the slow side for an embedded chip: the cost is what makes a
   weak password survive someone reading the flash.

   It is affordable only because the web layer caches a verified Authorization
   header rather than deriving the key again for every request. Basic
   authentication resends the header constantly, so without that cache this
   would run several times a minute, in the task that also serves the UI. */
constexpr int kPbkdf2Iterations = 20000;
constexpr size_t kSaltLen = 16;
constexpr size_t kHashLen = 32;

std::string to_hex(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(len * 2);
  char buf[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    out += buf;
  }
  return out;
}

bool from_hex(const std::string &hex, uint8_t *out, size_t out_len) {
  if (hex.size() != out_len * 2)
    return false;
  for (size_t i = 0; i < out_len; i++) {
    unsigned v;
    if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1)
      return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

/* ESP-IDF 6 ships mbedtls 4, where the standalone crypto primitives are gone
   and everything goes through PSA Crypto. mbedtls_pkcs5_pbkdf2_hmac_ext() and
   even mbedtls/sha256.h no longer exist. */
bool derive(const std::string &password, const uint8_t *salt, uint8_t *out) {
  psa_status_t rc = psa_crypto_init();
  if (rc != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_crypto_init failed: %d", static_cast<int>(rc));
    return false;
  }

  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  bool ok = false;
  do {
    rc = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (rc != PSA_SUCCESS) break;
    rc = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, kPbkdf2Iterations);
    if (rc != PSA_SUCCESS) break;
    rc = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, kSaltLen);
    if (rc != PSA_SUCCESS) break;
    rc = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        reinterpret_cast<const uint8_t *>(password.data()), password.size());
    if (rc != PSA_SUCCESS) break;
    rc = psa_key_derivation_output_bytes(&op, out, kHashLen);
    if (rc != PSA_SUCCESS) break;
    ok = true;
  } while (false);

  psa_key_derivation_abort(&op);
  if (!ok)
    ESP_LOGE(TAG, "PBKDF2 derivation failed: %d", static_cast<int>(rc));
  return ok;
}

/* Comparison that does not leak how many leading bytes matched. */
bool equal_ct(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); i++)
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

std::string get_str(const char *ns, const char *key, const std::string &fallback = "") {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return fallback;
  size_t len = 0;
  std::string out = fallback;
  if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
    out.resize(len);
    if (nvs_get_str(h, key, out.data(), &len) == ESP_OK)
      out.resize(len ? len - 1 : 0);  // drop the trailing NUL
    else
      out = fallback;
  }
  nvs_close(h);
  return out;
}

bool set_str(const char *ns, const char *key, const std::string &value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_str(h, key, value.c_str());
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

/* Longer texts (a CA certificate) go in a blob: NVS strings stop at 4000 bytes. */
std::string get_blob(const char *ns, const char *key) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return {};
  size_t len = 0;
  std::string out;
  if (nvs_get_blob(h, key, nullptr, &len) == ESP_OK && len > 0) {
    out.resize(len);
    if (nvs_get_blob(h, key, out.data(), &len) != ESP_OK)
      out.clear();
  }
  nvs_close(h);
  return out;
}

/* An empty value erases the key. */
bool set_blob(const char *ns, const char *key, const std::string &value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = value.empty() ? nvs_erase_key(h, key) : nvs_set_blob(h, key, value.data(), value.size());
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

uint8_t get_u8(const char *ns, const char *key, uint8_t fallback) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return fallback;
  uint8_t v = fallback;
  nvs_get_u8(h, key, &v);
  nvs_close(h);
  return v;
}

bool set_u8(const char *ns, const char *key, uint8_t value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_u8(h, key, value);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

}  // namespace

bool begin() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erasing (%s), reinitialising", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

std::string default_device_name() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "ohpanel-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return buf;
}

Wifi wifi() {
  Wifi w;
  w.ssid = get_str(kNsWifi, "ssid");
  w.password = get_str(kNsWifi, "pass");
  w.wpa3_only = get_u8(kNsWifi, "wpa3", 0) != 0;
  return w;
}

bool set_wifi(const std::string &ssid, const std::string &password, bool wpa3_only) {
  return set_str(kNsWifi, "ssid", ssid) && set_str(kNsWifi, "pass", password) &&
         set_u8(kNsWifi, "wpa3", wpa3_only && !password.empty() ? 1 : 0);
}

Ip ip() {
  Ip i;
  i.fixed = get_u8(kNsWifi, "ipfixed", 0) != 0;
  i.address = get_str(kNsWifi, "ipaddr");
  i.gateway = get_str(kNsWifi, "ipgw");
  i.dns1 = get_str(kNsWifi, "ipdns1");
  i.dns2 = get_str(kNsWifi, "ipdns2");
  i.prefix = get_u8(kNsWifi, "ipprefix", 24);
  i.ipv6 = get_u8(kNsWifi, "ipv6", 1) != 0;
  return i;
}

bool set_ip(const Ip &i) {
  return set_u8(kNsWifi, "ipfixed", i.fixed ? 1 : 0) && set_str(kNsWifi, "ipaddr", i.address) &&
         set_str(kNsWifi, "ipgw", i.gateway) && set_str(kNsWifi, "ipdns1", i.dns1) &&
         set_str(kNsWifi, "ipdns2", i.dns2) && set_u8(kNsWifi, "ipprefix", static_cast<uint8_t>(i.prefix)) &&
         set_u8(kNsWifi, "ipv6", i.ipv6 ? 1 : 0);
}

Auth auth() {
  Auth a;
  a.enabled = get_u8(kNsAuth, "enabled", 0) != 0;
  a.user = get_str(kNsAuth, "user", "admin");
  a.salt_hex = get_str(kNsAuth, "salt");
  a.hash_hex = get_str(kNsAuth, "hash");
  /* Protection without a stored password would lock everyone out with no way
     back, so it can never be on in that state. */
  if (!a.has_password())
    a.enabled = false;
  return a;
}

bool set_password(const std::string &user, const std::string &password) {
  if (password.empty()) {
    ESP_LOGI(TAG, "Password cleared, protection off");
    return set_str(kNsAuth, "salt", "") && set_str(kNsAuth, "hash", "") && set_u8(kNsAuth, "enabled", 0);
  }

  uint8_t salt[kSaltLen];
  esp_fill_random(salt, sizeof(salt));

  uint8_t hash[kHashLen];
  if (!derive(password, salt, hash))
    return false;

  return set_str(kNsAuth, "user", user.empty() ? "admin" : user) && set_str(kNsAuth, "salt", to_hex(salt, kSaltLen)) &&
         set_str(kNsAuth, "hash", to_hex(hash, kHashLen));
}

bool set_auth_enabled(bool enabled) {
  if (enabled && !auth().has_password()) {
    ESP_LOGW(TAG, "Refusing to enable protection without a password");
    return false;
  }
  return set_u8(kNsAuth, "enabled", enabled ? 1 : 0);
}

bool set_auth_hash(const std::string &user, const std::string &salt_hex, const std::string &hash_hex) {
  uint8_t salt[kSaltLen], hash[kHashLen];
  if (!from_hex(salt_hex, salt, kSaltLen) || !from_hex(hash_hex, hash, kHashLen))
    return false;
  return set_str(kNsAuth, "user", user.empty() ? "admin" : user) && set_str(kNsAuth, "salt", salt_hex) &&
         set_str(kNsAuth, "hash", hash_hex);
}

bool set_auth_user(const std::string &user) {
  return set_str(kNsAuth, "user", user.empty() ? "admin" : user);
}

bool verify_password(const std::string &user, const std::string &password) {
  Auth a = auth();
  if (!a.has_password())
    return false;
  if (a.user != user)
    return false;

  uint8_t salt[kSaltLen];
  if (!from_hex(a.salt_hex, salt, kSaltLen))
    return false;

  uint8_t hash[kHashLen];
  if (!derive(password, salt, hash))
    return false;

  return equal_ct(to_hex(hash, kHashLen), a.hash_hex);
}

OpenHab openhab() {
  OpenHab o;
  o.url = get_str(kNsOpenHab, "url");
  o.token = get_str(kNsOpenHab, "token");
  const uint8_t tls = get_u8(kNsOpenHab, "tls", 0);
  o.tls = tls == 1 ? Tls::Custom : tls == 2 ? Tls::Insecure : Tls::Bundle;
  o.ca = get_blob(kNsOpenHab, "ca");
  o.check_name = get_u8(kNsOpenHab, "cn", 1) != 0;
  return o;
}

Clock clock() {
  Clock c;
  c.server = get_str(kNsDev, "ntp", c.server);
  c.tz_name = get_str(kNsDev, "tz", c.tz_name);
  c.tz_posix = get_str(kNsDev, "tzposix", c.tz_posix);
  return c;
}

bool set_clock(const Clock &c) {
  return set_str(kNsDev, "ntp", c.server) && set_str(kNsDev, "tz", c.tz_name) &&
         set_str(kNsDev, "tzposix", c.tz_posix);
}

Sensor sensor() {
  Sensor s;
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READONLY, &h) == ESP_OK) {
    int32_t v = 0;
    if (nvs_get_i32(h, "toffset", &v) == ESP_OK)
      s.offset_tenths = static_cast<int>(v);
    nvs_close(h);
  }
  s.item = get_str(kNsDev, "titem");
  return s;
}

bool set_sensor(const Sensor &s) {
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_i32(h, "toffset", s.offset_tenths);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK && set_str(kNsDev, "titem", s.item);
}

bool relay(int index) { return get_u8(kNsDev, index == 0 ? "relay0" : "relay1", 0) != 0; }

bool set_relay(int index, bool on) { return set_u8(kNsDev, index == 0 ? "relay0" : "relay1", on ? 1 : 0); }

bool set_openhab(const OpenHab &o) {
  return set_str(kNsOpenHab, "url", o.url) && set_str(kNsOpenHab, "token", o.token) &&
         set_u8(kNsOpenHab, "tls", static_cast<uint8_t>(o.tls)) && set_blob(kNsOpenHab, "ca", o.ca) &&
         set_u8(kNsOpenHab, "cn", o.check_name ? 1 : 0);
}

Device device() {
  Device d;
  d.name = get_str(kNsDev, "name");
  if (d.name.empty())
    d.name = default_device_name();
  return d;
}

bool set_device_name(const std::string &name) { return set_str(kNsDev, "name", name); }

int tx_power_dbm() {
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READONLY, &h) != ESP_OK)
    return 0;
  int8_t v = 0;
  nvs_get_i8(h, "txpower", &v);
  nvs_close(h);
  return v;
}

bool set_tx_power_dbm(int dbm) {
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_i8(h, "txpower", static_cast<int8_t>(dbm));
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool factory_reset() {
  ESP_LOGW(TAG, "Factory reset: erasing all stored settings");
  bool ok = true;
  for (const char *ns : {kNsWifi, kNsAuth, kNsDev, kNsOpenHab}) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
      continue;
    /* Erase the namespace wholesale rather than key by key, so a key added
       later is not silently left behind by a reset that predates it. */
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK)
      err = nvs_commit(h);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Could not erase namespace \"%s\": %s", ns, esp_err_to_name(err));
      ok = false;
    }
    nvs_close(h);
  }
  return ok;
}

}  // namespace appcfg
