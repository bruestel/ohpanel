/*
   openHAB servers in the network, found by mDNS: openHAB announces itself as
   _openhab-server._tcp (http, port 8080 by default) and _openhab-server-ssl._tcp
   (https, 8443), each with "uri=/rest". Answers of one server -- by its
   address -- come together as one entry with both ports.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>

#include <esp_log.h>
#include <esp_netif_ip_addr.h>
#include <mdns.h>

#include <algorithm>

namespace appoh {
namespace {

const char *const TAG = "appoh";
constexpr uint32_t kQueryMs = 4000;   // both services at once: answers through an mDNS proxy come late
constexpr size_t kMaxResults = 12;
constexpr int kAskMs = 2000;

/* openHAB's version at `url` (its root document); empty: no answer from openHAB there. */
std::string version_at(const std::string &url) {
  appcfg::OpenHab cfg;
  cfg.url = url;
  const Response r = http_get(cfg, "/rest/", 8 * 1024, kAskMs);
  if (r.status == 401)
    return "?";
  if (r.status != 200)
    return "";
  cJSON *doc = cJSON_ParseWithLength(r.body.data(), r.body.size());
  const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "runtimeInfo"), "version");
  const std::string version = cJSON_IsString(v) ? v->valuestring : doc ? "?" : "";
  cJSON_Delete(doc);
  return version;
}

/* The server's IPv4 address of a result (its IPv6 one where it has none). */
std::string address_of(const mdns_result_t *r) {
  std::string v6;
  for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
    char buf[48];
    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
      snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a->addr.u_addr.ip4));
      return buf;
    }
    if (v6.empty()) {
      snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(a->addr.u_addr.ip6));
      v6 = std::string("[") + buf + "]";
    }
  }
  return v6;
}

/* The answers of a search into `out` (by address), freed. */
void collect(mdns_result_t *results, bool https, std::vector<Found> &out) {
  for (const mdns_result_t *r = results; r; r = r->next) {
    const std::string address = address_of(r);
    if (address.empty() || !r->port)
      continue;
    auto it = std::find_if(out.begin(), out.end(), [&](const Found &f) { return f.address == address; });
    if (it == out.end()) {
      out.push_back({});
      it = out.end() - 1;
      it->address = address;
    }
    if (it->name.empty() && r->instance_name)
      it->name = r->instance_name;
    if (it->hostname.empty() && r->hostname)
      it->hostname = std::string(r->hostname) + ".local";
    (https ? it->https_port : it->http_port) = r->port;
  }
  mdns_query_results_free(results);
}

}  // namespace

std::vector<Found> scan(bool check) {
  std::vector<Found> out;
  // both searches at once, each given the whole time
  mdns_search_once_t *plain = mdns_query_async_new(nullptr, "_openhab-server", "_tcp", MDNS_TYPE_PTR, kQueryMs, kMaxResults, nullptr);
  mdns_search_once_t *ssl = mdns_query_async_new(nullptr, "_openhab-server-ssl", "_tcp", MDNS_TYPE_PTR, kQueryMs, kMaxResults, nullptr);
  for (auto [search, https] : {std::pair{plain, false}, std::pair{ssl, true}}) {
    if (!search)
      continue;
    mdns_result_t *results = nullptr;
    uint8_t n = 0;
    mdns_query_async_get_results(search, kQueryMs + 500, &results, &n);
    collect(results, https, out);
    mdns_query_async_delete(search);
  }
  if (check)
    for (Found &f : out) {   // whether the panel gets an answer, over https (and its certificate) and http
      if (f.https_port)
        f.tls = discover(f.address, f.https_port, true);
      if (f.http_port) {
        const std::string v = version_at("http://" + f.address + ":" + std::to_string(f.http_port));
        f.http_ok = !v.empty();
        if (f.version.empty() && v != "?")
          f.version = v;
      }
      if (f.version.empty() && !f.tls.version.empty())
        f.version = f.tls.version;
    }
  ESP_LOGI(TAG, "mDNS: %u openHAB server(s)", static_cast<unsigned>(out.size()));
  return out;
}

}  // namespace appoh
