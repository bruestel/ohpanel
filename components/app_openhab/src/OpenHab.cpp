/*
   openHAB REST client.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

namespace appoh {
namespace {

const char *const TAG = "appoh";

constexpr int kTimeoutMs = 5000;

/* The root document is a few kilobytes; anything far beyond is not openHAB. */
constexpr size_t kMaxProbeBody = 16 * 1024;

}  // namespace

void apply_tls(esp_http_client_config_t &hc, const appcfg::OpenHab &cfg) {
  // e.g. a certificate for "openhab.local" while the URL has the IP address
  hc.skip_cert_common_name_check = !cfg.check_name;
  switch (cfg.tls) {
    case appcfg::Tls::Custom:
      hc.cert_pem = cfg.ca.c_str();  // NUL-terminated: the length is taken from it
      break;
    case appcfg::Tls::Insecure:
      // neither a bundle nor a CA: with CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
      // the connection takes any certificate
      break;
    default:
      hc.crt_bundle_attach = esp_crt_bundle_attach;
  }
}

/* A small blocking GET. Never asks for gzip: esp_http_client does not decode
   it, and openHAB compresses JSON for clients that ask. */
Response http_get(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms) {
  Response r;
  const std::string url = cfg.url + path;

  esp_http_client_config_t hc = {};
  hc.url = url.c_str();
  hc.timeout_ms = timeout_ms > 0 ? timeout_ms : kTimeoutMs;
  apply_tls(hc, cfg);
  esp_http_client_handle_t client = esp_http_client_init(&hc);
  if (!client) {
    r.err = ESP_ERR_NO_MEM;
    return r;
  }

  esp_http_client_set_header(client, "Accept", "application/json");
  const std::string auth = "Bearer " + cfg.token;
  if (!cfg.token.empty())
    esp_http_client_set_header(client, "Authorization", auth.c_str());

  r.err = esp_http_client_open(client, 0);
  if (r.err == ESP_OK) {
    esp_http_client_fetch_headers(client);
    r.status = esp_http_client_get_status_code(client);
    char buf[1024];
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
      if (r.body.size() + n > limit) {
        r.err = ESP_ERR_INVALID_SIZE;
        break;
      }
      r.body.append(buf, n);
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return r;
}

bool fetch(const std::string &url, size_t limit, std::string &body, std::string *error) {
  const appcfg::OpenHab cfg = appcfg::openhab();
  const bool own = url.rfind("/", 0) == 0 || (cfg.configured() && url.rfind(cfg.url + "/", 0) == 0);
  const std::string full = url.rfind("/", 0) == 0 ? cfg.url + url : url;
  if (full.rfind("http://", 0) != 0 && full.rfind("https://", 0) != 0) {
    if (error) *error = "not an http(s) address";
    return false;
  }
  esp_http_client_config_t hc = {};
  hc.url = full.c_str();
  hc.timeout_ms = kTimeoutMs;
  hc.max_redirection_count = 3;
  if (own)
    apply_tls(hc, cfg);
  else
    hc.crt_bundle_attach = esp_crt_bundle_attach;   // somewhere on the internet: the usual certificates
  esp_http_client_handle_t client = esp_http_client_init(&hc);
  if (!client) {
    if (error) *error = "no memory";
    return false;
  }
  const std::string auth = "Bearer " + cfg.token;
  if (own && !cfg.token.empty())
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  body.clear();
  esp_err_t err = esp_http_client_open(client, 0);
  int status = 0;
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    // redirected (a CDN): followed, up to max_redirection_count
    while ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
           esp_http_client_set_redirection(client) == ESP_OK) {
      esp_http_client_close(client);
      if ((err = esp_http_client_open(client, 0)) != ESP_OK)
        break;
      esp_http_client_fetch_headers(client);
      status = esp_http_client_get_status_code(client);
    }
    char buf[1024];
    int n;
    while (err == ESP_OK && status == 200 && (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
      if (body.size() + n > limit) {
        err = ESP_ERR_INVALID_SIZE;
        break;
      }
      body.append(buf, n);
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (err == ESP_OK && status == 200)
    return true;
  if (error)
    *error = err == ESP_ERR_INVALID_SIZE ? "larger than " + std::to_string(limit / 1024) + " KB"
             : err != ESP_OK            ? std::string(esp_err_to_name(err))
                                        : "HTTP " + std::to_string(status);
  body.clear();
  return false;
}

namespace {

std::string json_str(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}

}  // namespace

std::string normalize_url(const std::string &url) {
  std::string u = url;
  while (!u.empty() && (u.back() == '/' || u.back() == ' '))
    u.pop_back();
  while (!u.empty() && u.front() == ' ')
    u.erase(0, 1);
  if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0)
    return {};
  return u;
}

ProbeResult probe(const appcfg::OpenHab &cfg) {
  ProbeResult p;
  if (!cfg.configured()) {
    p.error = "no openHAB URL configured";
    return p;
  }

  /* Step 1: the root document says whether this is openHAB at all, and which
     version, locale and time zone it runs with. An invalid token is refused
     here already: openHAB rejects bad credentials on every endpoint, even where
     anonymous access would be allowed. */
  Response root = http_get(cfg, "/rest/", kMaxProbeBody);
  p.http_status = root.status;
  if (root.err != ESP_OK && root.status == 0) {
    p.error = std::string("server not reachable (") + esp_err_to_name(root.err) + ")";
    return p;
  }
  if (root.status == 401) {
    p.error = "the token was rejected";
    return p;
  }
  if (root.status != 200) {
    p.error = "unexpected answer from the server (HTTP " + std::to_string(root.status) + ")";
    return p;
  }

  cJSON *doc = cJSON_ParseWithLength(root.body.data(), root.body.size());
  const cJSON *runtime = doc ? cJSON_GetObjectItem(doc, "runtimeInfo") : nullptr;
  if (!runtime) {
    cJSON_Delete(doc);
    p.error = "the server answered, but it does not look like openHAB";
    return p;
  }
  p.version = json_str(runtime, "version");
  p.locale = json_str(doc, "locale");
  p.timezone = json_str(doc, "timezone");
  cJSON_Delete(doc);

  /* Step 2: reading items needs the user role, which an anonymous client only
     has while openHAB's "implicit user role" is on. A type that does not exist
     keeps the answer to "[]" whatever the size of the installation. */
  Response items = http_get(cfg, "/rest/items?type=OHpanelProbe&fields=name&metadata=", kMaxProbeBody);
  p.http_status = items.status;
  if (items.status == 401 || items.status == 403) {
    p.error = cfg.token.empty() ? "openHAB requires a token to read items"
                                : "the token may not read items (it needs the user role)";
    return p;
  }
  if (items.status != 200) {
    p.error = "reading items failed (HTTP " + std::to_string(items.status) + ")";
    return p;
  }

  ESP_LOGI(TAG, "openHAB %s at %s, locale %s, time zone %s, %s", p.version.c_str(), cfg.url.c_str(),
           p.locale.c_str(), p.timezone.c_str(), cfg.token.empty() ? "anonymous" : "with token");
  p.ok = true;
  return p;
}

}  // namespace appoh
