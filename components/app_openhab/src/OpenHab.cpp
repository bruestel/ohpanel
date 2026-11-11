/*
   openHAB REST client.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <esp_timer.h>

#include <algorithm>
#include <functional>
#include <mutex>

namespace appoh {
namespace {

const char *const TAG = "appoh";

constexpr int kTimeoutMs = 5000;

/* The root document is a few kilobytes; anything far beyond is not openHAB. */
constexpr size_t kMaxProbeBody = 16 * 1024;

/* Room in `body` for `more` bytes, at most `limit` in all: what the response announced (Content-Length) at once,
   else doubling. False when no free block is that large: a std::string that cannot grow aborts the firmware (no
   exceptions), as a 300 KB cover did once the PSRAM had no 512 KB in one piece. */
bool make_room(std::string &body, size_t more, size_t limit, int64_t announced) {
  const size_t need = body.size() + more;
  if (need <= body.capacity())
    return true;
  size_t want = std::max(need, body.capacity() * 2);
  if (announced > 0 && static_cast<size_t>(announced) >= need)
    want = static_cast<size_t>(announced);
  want = std::min(want, limit);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < want + 64) {
    ESP_LOGW(TAG, "No %u bytes in one piece for a response", static_cast<unsigned>(want));
    return false;
  }
  body.reserve(want);
  return true;
}

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
bool same_settings(const appcfg::OpenHab &a, const appcfg::OpenHab &b) {
  return a.url == b.url && a.token == b.token && a.tls == b.tls && a.ca == b.ca && a.check_name == b.check_name;
}

namespace {
/* A client for GETs from `cfg` (its credentials, JSON asked for); null without memory. */
esp_http_client_handle_t get_client(const appcfg::OpenHab &cfg, const std::string &url, int timeout_ms,
                                    bool keep_session) {
  esp_http_client_config_t hc = {};
  hc.url = url.c_str();
  hc.timeout_ms = timeout_ms > 0 ? timeout_ms : kTimeoutMs;
  apply_tls(hc, cfg);
  hc.save_client_session = keep_session;
  esp_http_client_handle_t client = esp_http_client_init(&hc);
  if (!client)
    return nullptr;
  esp_http_client_set_header(client, "Accept", "application/json");
  if (!cfg.token.empty()) {
    const std::string auth = "Bearer " + cfg.token;   // copied by the client
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  }
  return client;
}

/* The response of a client opened for it, into r (status, body up to `limit`). */
void read_response(esp_http_client_handle_t client, size_t limit, Response &r) {
  const int64_t announced = esp_http_client_fetch_headers(client);
  r.status = esp_http_client_get_status_code(client);
  char buf[1024];
  int n;
  while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
    if (r.body.size() + n > limit) {
      r.err = ESP_ERR_INVALID_SIZE;
      break;
    }
    if (!make_room(r.body, n, limit, announced)) {
      r.err = ESP_ERR_NO_MEM;
      r.body.clear();   // not a part of it to parse
      break;
    }
    r.body.append(buf, n);
  }
}

/* http_get_resumed's client, kept between GETs for the TLS session saved on it (not for the connection: closed
   after each); one GET at a time. */
std::mutex g_resume_mx;
esp_http_client_handle_t g_resume = nullptr;
appcfg::OpenHab g_resume_cfg;   // what g_resume was made with; holds the CA it points into (set only without one)
int64_t g_resume_made_at = 0;    // its full handshake: the session each GET resumes dates from it
/* A server forgets a TLS session a while after its full handshake, however often it was resumed since (openHAB's
   Jetty: a day; a proxy before it maybe sooner): an hour old, the next GET makes it anew. */
constexpr int64_t kResumeRenewUs = 3600 * 1000000LL;
}  // namespace

Response http_get(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms) {
  Response r;
  esp_http_client_handle_t client = get_client(cfg, cfg.url + path, timeout_ms, false);
  if (!client) {
    r.err = ESP_ERR_NO_MEM;
    return r;
  }
  r.err = esp_http_client_open(client, 0);
  if (r.err == ESP_OK)
    read_response(client, limit, r);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return r;
}

Response http_get_resumed(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms) {
  if (cfg.url.rfind("https:", 0) != 0)
    return http_get(cfg, path, limit, timeout_ms);   // no TLS, nothing to resume
  const std::string url = cfg.url + path;
  const int timeout = timeout_ms > 0 ? timeout_ms : kTimeoutMs;
  std::lock_guard<std::mutex> lock(g_resume_mx);
  // other settings, or an old session: a new client, a full handshake
  if (g_resume && (!same_settings(cfg, g_resume_cfg) || esp_timer_get_time() - g_resume_made_at > kResumeRenewUs)) {
    esp_http_client_cleanup(g_resume);
    g_resume = nullptr;
  }
  const bool resumed = g_resume != nullptr;
  if (!g_resume) {
    g_resume_cfg = cfg;
    g_resume = get_client(g_resume_cfg, url, timeout, true);
    if (!g_resume)
      return Response{0, {}, ESP_ERR_NO_MEM};
    g_resume_made_at = esp_timer_get_time();
  } else {
    esp_http_client_set_url(g_resume, url.c_str());
    esp_http_client_set_timeout_ms(g_resume, timeout);
  }
  Response r;
  const int64_t began = esp_timer_get_time();
  r.err = esp_http_client_open(g_resume, 0);
  if (r.err == ESP_OK)
    read_response(g_resume, limit, r);
  esp_http_client_close(g_resume);   // no keep-alive: only the session stays
  /* No connection on a resumed session, and soon: perhaps the server did not take the session -- once more with a
     full handshake, on a client of its own, which keeps its session only if it gets through. Not after a timeout:
     openHAB away, a second one would only wait as long again. */
  if (r.err != ESP_ERR_HTTP_CONNECT || !resumed || esp_timer_get_time() - began > timeout * 1000LL / 2)
    return r;
  ESP_LOGW(TAG, "GET %s: no connection on the saved TLS session, once more with a full handshake", path.c_str());
  // the same settings as g_resume's (else it would be gone): the new client points into the same CA
  esp_http_client_handle_t fresh = get_client(g_resume_cfg, url, timeout, true);
  if (!fresh)
    return r;
  Response again;
  again.err = esp_http_client_open(fresh, 0);
  if (again.err == ESP_OK)
    read_response(fresh, limit, again);
  esp_http_client_close(fresh);
  if (again.err == ESP_ERR_HTTP_CONNECT) {   // not this way either: openHAB away, the old session kept
    esp_http_client_cleanup(fresh);
    return again;
  }
  esp_http_client_cleanup(g_resume);   // the new session from now on
  g_resume = fresh;
  g_resume_made_at = esp_timer_get_time();
  return again;
}

namespace {

/* fetch's work: what comes handed to `part` (the announced size with it, -1 unknown) as it comes; ESP_OK from it
   goes on, anything else stops with that error. */
bool fetch_parts(const std::string &url, size_t limit,
                 const std::function<esp_err_t(const char *, size_t, int64_t)> &part, std::string *error) {
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
    const int64_t announced = esp_http_client_get_content_length(client);
    char buf[1024];
    int n;
    size_t got = 0;
    while (err == ESP_OK && status == 200 && (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
      if (got + n > limit) {
        err = ESP_ERR_INVALID_SIZE;
        break;
      }
      err = part(buf, n, announced);
      got += n;
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (err == ESP_OK && status == 200)
    return true;
  if (error)
    *error = err == ESP_ERR_INVALID_SIZE ? "larger than " + std::to_string(limit / 1024) + " KB"
             : err == ESP_ERR_NO_MEM    ? "no memory for it in one piece"
             : err != ESP_OK            ? std::string(esp_err_to_name(err))
                                        : "HTTP " + std::to_string(status);
  return false;
}

}  // namespace

bool fetch(const std::string &url, size_t limit, std::string &body, std::string *error) {
  body.clear();
  const bool ok = fetch_parts(url, limit, [&](const char *data, size_t n, int64_t announced) {
    if (!make_room(body, n, limit, announced))
      return ESP_ERR_NO_MEM;
    body.append(data, n);
    return ESP_OK;
  }, error);
  if (!ok)
    body.clear();
  return ok;
}

bool fetch(const std::string &url, size_t limit, const std::function<bool(const char *, size_t)> &part,
           std::string *error) {
  return fetch_parts(url, limit, [&](const char *data, size_t n, int64_t) {
    return part(data, n) ? ESP_OK : ESP_ERR_INVALID_STATE;
  }, error);
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
