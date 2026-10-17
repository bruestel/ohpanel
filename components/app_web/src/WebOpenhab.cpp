/*
   The openHAB routes: its settings and their test, finding servers, items, options and states.
*/

#include "WebDetail.h"

#include <map>
#include <mutex>

namespace appweb {
namespace detail {

const char *tls_name(appcfg::Tls t) {
  return t == appcfg::Tls::Custom ? "custom" : t == appcfg::Tls::Insecure ? "insecure" : "bundle";
}

/* The certificates of a PEM text: false (and why) if mbedTLS cannot read
   them; else `summary` names the first one's subject and when it expires. */
bool read_ca(const std::string &pem, std::string &summary, std::string &error) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const int rc = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1);
  bool ok = rc == 0 && crt.version != 0;
  if (ok) {
    char subject[160] = "";
    mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    char until[32];
    snprintf(until, sizeof(until), "%04d-%02d-%02d", crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
    int count = 0;
    for (const mbedtls_x509_crt *c = &crt; c && c->version; c = c->next)
      count++;
    summary = std::string(subject) + ", valid until " + until + (count > 1 ? " (+" + std::to_string(count - 1) + " more)" : "");
  } else {
    error = "the CA is not a PEM certificate (-----BEGIN CERTIFICATE----- ...)";
  }
  mbedtls_x509_crt_free(&crt);
  return ok;
}

/* The token is never sent back -- only whether one is stored. The CA is
   public; the page shows what it is. */
esp_err_t get_openhab(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const appcfg::OpenHab o = appcfg::openhab();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "url", o.url.c_str());
  cJSON_AddBoolToObject(r, "has_token", !o.token.empty());
  cJSON_AddStringToObject(r, "tls", tls_name(o.tls));
  cJSON_AddStringToObject(r, "ca", o.ca.c_str());
  cJSON_AddBoolToObject(r, "check_name", o.check_name);
  std::string summary, error;
  if (!o.ca.empty())
    cJSON_AddStringToObject(r, "ca_info", read_ca(o.ca, summary, error) ? summary.c_str() : error.c_str());
  return send_json(req, r);
}

bool openhab_from_body(httpd_req_t *req, appcfg::OpenHab &o, std::string &error) {
  o = appcfg::openhab();
  std::string body;
  if (!read_body(req, body, 2048 + 2 * appcfg::kMaxCaPem)) {
    error = "bad body";
    return false;
  }
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    error = "invalid JSON";
    return false;
  }
  const bool ok = openhab_from_json(root, o, error);
  cJSON_Delete(root);
  return ok;
}

/* The same from a JSON object, on top of what `o` holds. */
bool openhab_from_json(const cJSON *root, appcfg::OpenHab &o, std::string &error) {
  const cJSON *url = cJSON_GetObjectItem(root, "url");
  const cJSON *token = cJSON_GetObjectItem(root, "token");
  const cJSON *tls = cJSON_GetObjectItem(root, "tls");
  const cJSON *ca = cJSON_GetObjectItem(root, "ca");
  const cJSON *check_name = cJSON_GetObjectItem(root, "check_name");
  if (cJSON_IsBool(check_name))
    o.check_name = cJSON_IsTrue(check_name);
  if (cJSON_IsString(url))
    o.url = url->valuestring;
  if (cJSON_IsString(token))
    o.token = token->valuestring;
  if (cJSON_IsString(ca))
    o.ca = ca->valuestring;
  std::string mode = cJSON_IsString(tls) ? tls->valuestring : tls_name(o.tls);

  if (mode == "custom") o.tls = appcfg::Tls::Custom;
  else if (mode == "insecure") o.tls = appcfg::Tls::Insecure;
  else if (mode == "bundle") o.tls = appcfg::Tls::Bundle;
  else {
    error = "tls must be bundle, custom or insecure";
    return false;
  }
  while (!o.ca.empty() && (o.ca.back() == '\n' || o.ca.back() == '\r' || o.ca.back() == ' '))
    o.ca.pop_back();
  if (!o.ca.empty())
    o.ca += "\n";
  if (o.ca.size() > appcfg::kMaxCaPem) {
    error = "the CA must be at most " + std::to_string(appcfg::kMaxCaPem / 1024) + " KB";
    return false;
  }
  std::string summary;
  if (!o.ca.empty() && !read_ca(o.ca, summary, error))
    return false;
  if (o.tls == appcfg::Tls::Custom && o.ca.empty()) {
    error = "checking against your own CA needs the CA";
    return false;
  }

  if (!o.url.empty()) {
    o.url = appoh::normalize_url(o.url);
    if (o.url.empty()) {
      error = "the URL must start with http:// or https://";
      return false;
    }
  }
  return true;
}

esp_err_t post_openhab(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  appcfg::OpenHab o;
  std::string error;
  if (!openhab_from_body(req, o, error)) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error.c_str());
    return send_json(req, r);
  }
  if (!appcfg::set_openhab(o)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return send_raw_json(req, "{\"error\":\"store failed\"}");
  }
  appoh::reconfigure();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddStringToObject(r, "url", o.url.c_str());
  return send_json(req, r);
}

/* ?q=<words>&type=<prefix>&offset=<n>&refresh=1 -- searches openHAB's item
   list, 50 matches from the offset on (the editor loads more as it scrolls), for the
   configuration editor. Proxied rather than called from the browser, because
   openHAB does not allow cross-origin requests by default. */
esp_err_t get_openhab_items(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  constexpr size_t kLimit = 50;
  std::vector<appoh::ItemInfo> found;
  size_t matches = 0;
  std::string error;
  if (!appoh::search_items(query_param(req, "q"), query_param(req, "type"), kLimit, found, matches, error,
                           query_param(req, "refresh") == "1",
                           std::strtoul(query_param(req, "offset").c_str(), nullptr, 10))) {
    httpd_resp_set_status(req, "502 Bad Gateway");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error.c_str());
    return send_json(req, r);
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "matches", matches);
  cJSON *list = cJSON_AddArrayToObject(r, "items");
  for (const auto &i : found) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", i.name.c_str());
    cJSON_AddStringToObject(o, "label", i.label.c_str());
    cJSON_AddStringToObject(o, "type", i.type.c_str());
    cJSON_AddItemToArray(list, o);
  }
  return send_json(req, r);
}

/* ?item=<name>&refresh=1 -- an item's options as the panel has them (a
   station catalogue: value, label, and from its "ohpanel" metadata icon and
   logo code), for the station editor. Not waiting for openHAB: "known" is
   false while they are being fetched -- asked again shortly, they are there. */
esp_err_t get_openhab_options(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const std::string item = query_param(req, "item");
  std::vector<appoh::Option> options;
  const bool known = appoh::options(item, options, query_param(req, "refresh") == "1");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "known", known);
  // the item's range (stateDescription), where openHAB gives one: the light page's colour temperature ends
  appoh::Limits range;
  appoh::limits(item, range);
  if (range.has_min) cJSON_AddNumberToObject(r, "min", range.min);
  if (range.has_max) cJSON_AddNumberToObject(r, "max", range.max);
  if (range.has_step) cJSON_AddNumberToObject(r, "step", range.step);
  cJSON *list = cJSON_AddArrayToObject(r, "options");
  for (const auto &o : options) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "value", o.value.c_str());
    cJSON_AddStringToObject(e, "label", o.label.c_str());
    if (!o.icon.empty()) cJSON_AddStringToObject(e, "icon", o.icon.c_str());
    if (!o.image.empty()) cJSON_AddStringToObject(e, "image", o.image.c_str());
    if (!o.short_name.empty()) cJSON_AddStringToObject(e, "short", o.short_name.c_str());
    if (!o.name.empty()) cJSON_AddStringToObject(e, "name", o.name.c_str());
    cJSON_AddItemToArray(list, e);
  }
  // a group's members with their metadata (an entity page's group, for the preview)
  std::vector<appoh::Member> members;
  appoh::members(item, members);
  const auto add_metadata = [](cJSON *to, const std::map<std::string, appoh::Metadata> &all) {
    cJSON *md = cJSON_AddObjectToObject(to, "metadata");
    for (const auto &[ns, data] : all) {
      cJSON *n = cJSON_AddObjectToObject(md, ns.c_str());
      cJSON_AddStringToObject(n, "value", data.value.c_str());
      cJSON *c = cJSON_AddObjectToObject(n, "config");
      for (const auto &[k, v] : data.config)
        cJSON_AddStringToObject(c, k.c_str(), v.c_str());
    }
  };
  cJSON *ml = cJSON_AddArrayToObject(r, "members");
  for (const auto &m : members) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", m.name.c_str());
    cJSON_AddStringToObject(e, "label", m.label.c_str());
    add_metadata(e, m.metadata);
    cJSON_AddItemToArray(ml, e);
  }
  // the item's own (a cover's style and presets)
  std::map<std::string, appoh::Metadata> own;
  if (appoh::metadata(item, own))
    add_metadata(r, own);
  return send_json(req, r);
}

/* The last state of every subscribed item, as received from openHAB. */
esp_err_t get_openhab_states(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  return send_raw_json(req, appoh::states_json());
}

/* openHAB over https on the host of an address ({"url": "http://host:8080"}),
   whatever its protocol: what appoh::discover finds, for the settings page to
   offer https with the certificate found. */
esp_err_t post_openhab_discover(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  if (!read_body(req, body, 2048))
    return send_raw_json(req, "{\"found\":false,\"error\":\"bad request\"}");
  cJSON *in = cJSON_Parse(body.c_str());
  std::string url = in && cJSON_IsString(cJSON_GetObjectItem(in, "url")) ? cJSON_GetObjectItem(in, "url")->valuestring : "";
  cJSON_Delete(in);
  // host and port of scheme://host[:port][/...]
  if (const size_t at = url.find("://"); at != std::string::npos)
    url = url.substr(at + 3);
  url = url.substr(0, url.find('/'));
  std::string host = url;
  int port = 0;
  if (const size_t colon = url.rfind(':'); colon != std::string::npos && url.find(']') == std::string::npos) {
    host = url.substr(0, colon);
    port = atoi(url.c_str() + colon + 1);
  }
  cJSON *r = cJSON_CreateObject();
  add_discovery(r, host, appoh::discover(host, port));
  return send_json(req, r);
}

/* What appoh::discover found, into `r`. */
void add_discovery(cJSON *r, const std::string &host, const appoh::Discovery &d) {
  cJSON_AddBoolToObject(r, "found", d.found);
  if (d.found) {
    cJSON_AddStringToObject(r, "host", host.c_str());
    cJSON_AddNumberToObject(r, "port", d.port);
    cJSON_AddStringToObject(r, "version", d.version.c_str());
    cJSON_AddBoolToObject(r, "public", d.public_ca);
    cJSON_AddBoolToObject(r, "importable", d.importable);
    cJSON_AddBoolToObject(r, "self_signed", d.self_signed);
    cJSON_AddBoolToObject(r, "name_ok", d.name_ok);
    cJSON *names = cJSON_AddArrayToObject(r, "names");
    for (const auto &n : d.names)
      cJSON_AddItemToArray(names, cJSON_CreateString(n.c_str()));
    if (!d.subject.empty()) cJSON_AddStringToObject(r, "subject", d.subject.c_str());
    if (!d.valid_to.empty()) cJSON_AddStringToObject(r, "valid_to", d.valid_to.c_str());
    if (d.importable) {
      cJSON_AddStringToObject(r, "pem", d.pem.c_str());
      cJSON_AddStringToObject(r, "fingerprint", d.fingerprint.c_str());
    }
  }
}

/* openHAB servers in the network (appoh::scan, mDNS), each with what its https port answers (appoh::discover:
   its certificate), for the settings page to offer -- one answer, so the page shows it all at once. The last
   search is kept a while (a page opened again shows it at once); ?refresh=1 searches anew. */
constexpr int64_t kScanKeepUs = 120LL * 1000 * 1000;

std::mutex g_scan_mx;

std::vector<appoh::Found> g_scan;

int64_t g_scan_at = -1;

esp_err_t get_openhab_scan(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::vector<appoh::Found> found;
  {
    std::lock_guard<std::mutex> lock(g_scan_mx);
    const int64_t now = esp_timer_get_time();
    if (g_scan_at < 0 || now - g_scan_at > kScanKeepUs || query_param(req, "refresh") == "1") {
      g_scan = appoh::scan(true);
      g_scan_at = esp_timer_get_time();
    }
    found = g_scan;
  }
  cJSON *r = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(r, "servers");
  for (const appoh::Found &f : found) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", f.name.c_str());
    cJSON_AddStringToObject(e, "hostname", f.hostname.c_str());
    cJSON_AddStringToObject(e, "address", f.address.c_str());
    cJSON_AddNumberToObject(e, "http", f.http_port);
    cJSON_AddNumberToObject(e, "https", f.https_port);
    if (f.https_port)
      add_discovery(cJSON_AddObjectToObject(e, "tls"), f.address, f.tls);
    cJSON_AddBoolToObject(e, "http_ok", f.http_ok);
    cJSON_AddStringToObject(e, "version", f.version.c_str());
    cJSON_AddItemToArray(list, e);
  }
  return send_json(req, r);
}

esp_err_t post_openhab_test(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  appcfg::OpenHab o;
  std::string error;
  if (!openhab_from_body(req, o, error)) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error.c_str());
    return send_json(req, r);
  }

  const appoh::ProbeResult p = appoh::probe(o);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", p.ok);
  cJSON_AddNumberToObject(r, "http_status", p.http_status);
  if (!p.ok)
    cJSON_AddStringToObject(r, "error", p.error.c_str());
  cJSON_AddStringToObject(r, "version", p.version.c_str());
  cJSON_AddStringToObject(r, "locale", p.locale.c_str());
  cJSON_AddStringToObject(r, "timezone", p.timezone.c_str());
  return send_json(req, r);
}

}  // namespace detail
}  // namespace appweb
