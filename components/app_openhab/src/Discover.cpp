/*
   Finding openHAB over https on a host the settings name -- whatever
   protocol they use -- and the certificate it answers with, so the web
   interface can offer to switch to https and trust exactly that certificate.

   For each port tried: openHAB at all (its root document, the certificate not
   checked), then whether the built-in CAs vouch for it (nothing to import
   then), else the certificates it sends: taken during the handshake by a
   verify callback that accepts them all -- this build keeps no peer
   certificate after the handshake. Only a chain that ends in a self-signed
   certificate can be trusted by the panel: that one is offered (PEM, its
   names, until when it is valid, its SHA-256 fingerprint).

   Trust on first use: nothing here proves that what answers is the user's
   openHAB. The fingerprint is shown so it can be compared.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

namespace appoh {
namespace {

const char *const TAG = "appoh";
constexpr size_t kMaxRootBody = 8 * 1024;
constexpr int kHandshakeMs = 2500;
constexpr int kAskMs = 2000;   // a host that does not answer is given up soon: the page waits for it

/* The certificates of one handshake, by depth (0: the server's own). One
   discovery at a time: the callback has no other way to find them. */
std::mutex g_grab_mx;
std::vector<std::vector<uint8_t>> *g_grab = nullptr;

int grab_verify(void *, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
  if (g_grab && depth >= 0 && depth < 8) {
    if (g_grab->size() <= static_cast<size_t>(depth))
      g_grab->resize(depth + 1);
    (*g_grab)[depth].assign(crt->raw.p, crt->raw.p + crt->raw.len);
  }
  *flags = 0;   // taken as it is: only read, never trusted from here
  return 0;
}

/* As esp_crt_bundle_attach does: an empty CA chain (mbedTLS refuses to verify without one, before it asks the
   callback) and the callback. */
mbedtls_x509_crt g_no_ca;

esp_err_t grab_attach(void *conf) {
  static bool ready = false;
  if (!ready) {
    mbedtls_x509_crt_init(&g_no_ca);
    ready = true;
  }
  auto *c = static_cast<mbedtls_ssl_config *>(conf);
  mbedtls_ssl_conf_ca_chain(c, &g_no_ca, nullptr);
  mbedtls_ssl_conf_verify(c, grab_verify, nullptr);
  return ESP_OK;
}

std::string base64_pem(const std::vector<uint8_t> &der) {
  static const char *const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  for (size_t i = 0; i < der.size(); i += 3) {
    const uint32_t n = der[i] << 16 | (i + 1 < der.size() ? der[i + 1] << 8 : 0) | (i + 2 < der.size() ? der[i + 2] : 0);
    b64 += kAlphabet[n >> 18 & 63];
    b64 += kAlphabet[n >> 12 & 63];
    b64 += i + 1 < der.size() ? kAlphabet[n >> 6 & 63] : '=';
    b64 += i + 2 < der.size() ? kAlphabet[n & 63] : '=';
  }
  std::string pem = "-----BEGIN CERTIFICATE-----\n";
  for (size_t i = 0; i < b64.size(); i += 64)
    pem += b64.substr(i, 64) + "\n";
  return pem + "-----END CERTIFICATE-----\n";
}

std::string fingerprint(const std::vector<uint8_t> &der) {
  uint8_t hash[32];
  size_t len = 0;
  if (psa_hash_compute(PSA_ALG_SHA_256, der.data(), der.size(), hash, sizeof(hash), &len) != PSA_SUCCESS)
    return "";
  std::string out;
  char hex[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(hex, sizeof(hex), i ? ":%02X" : "%02X", hash[i]);
    out += hex;
  }
  return out;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

/* Whether a certificate name ("host", "*.example.org") covers `host`. */
bool name_covers(const std::string &pattern, const std::string &host) {
  const std::string p = lower(pattern), h = lower(host);
  if (p.rfind("*.", 0) == 0) {
    const size_t dot = h.find('.');
    return dot != std::string::npos && h.substr(dot) == p.substr(1);
  }
  return p == h;
}

/* The names a certificate is issued for: its DNS and IP subject alternative
   names, else its common name. */
std::vector<std::string> names_of(const mbedtls_x509_crt &crt) {
  std::vector<std::string> out;
  for (const mbedtls_x509_sequence *s = &crt.subject_alt_names; s && s->buf.p; s = s->next) {
    mbedtls_x509_subject_alternative_name san;
    if (mbedtls_x509_parse_subject_alt_name(&s->buf, &san) != 0)
      continue;
    const mbedtls_x509_buf &b = san.san.unstructured_name;
    if (san.type == MBEDTLS_X509_SAN_DNS_NAME) {
      out.emplace_back(reinterpret_cast<const char *>(b.p), b.len);
    } else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && b.len == 4) {
      char ip[16];
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b.p[0], b.p[1], b.p[2], b.p[3]);
      out.emplace_back(ip);
    }
    mbedtls_x509_free_subject_alt_name(&san);
  }
  if (out.empty()) {
    char dn[200] = "";
    mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject);
    const char *cn = strstr(dn, "CN=");
    if (cn) {
      std::string name(cn + 3);
      out.push_back(name.substr(0, name.find(',')));
    }
  }
  return out;
}

/* openHAB's root document at `url`, the certificate checked as `tls` says:
   the version (empty: not openHAB); `reached`: a TLS connection came about. */
std::string openhab_at(const std::string &url, appcfg::Tls tls, bool &reached) {
  appcfg::OpenHab cfg;
  cfg.url = url;
  cfg.tls = tls;
  cfg.check_name = tls != appcfg::Tls::Insecure;
  const Response r = http_get(cfg, "/rest/", kMaxRootBody, kAskMs);
  reached = r.status != 0;
  if (r.status != 200)
    return r.status == 401 ? "?" : "";   // openHAB that wants a token for its root document too
  cJSON *doc = cJSON_ParseWithLength(r.body.data(), r.body.size());
  const cJSON *runtime = cJSON_GetObjectItem(doc, "runtimeInfo");
  const cJSON *v = cJSON_GetObjectItem(runtime, "version");
  const std::string version = cJSON_IsString(v) ? v->valuestring : runtime ? "?" : "";
  cJSON_Delete(doc);
  return version;
}

/* The certificates `host`:`port` sends, by depth; empty if no handshake came about. */
std::vector<std::vector<uint8_t>> chain_of(const std::string &host, int port) {
  std::vector<std::vector<uint8_t>> chain;
  std::lock_guard<std::mutex> lock(g_grab_mx);
  g_grab = &chain;
  esp_tls_cfg_t tc = {};
  tc.timeout_ms = kHandshakeMs;
  tc.crt_bundle_attach = grab_attach;   // the hook the CA bundle uses: here a callback that reads them all
  tc.skip_common_name = true;
  esp_tls_t *tls = esp_tls_init();
  if (tls) {
    if (esp_tls_conn_new_sync(host.c_str(), static_cast<int>(host.size()), port, &tc, tls) != 1)
      chain.clear();
    esp_tls_conn_destroy(tls);
  }
  g_grab = nullptr;
  return chain;
}

}  // namespace

Discovery discover(const std::string &host, int port, bool only) {
  Discovery d;
  if (host.empty()) {
    d.error = "no host";
    return d;
  }
  std::vector<int> ports;
  if (port > 0 && port != 80 && port != 8080)
    ports.push_back(port);
  for (const int p : {443, 8443})
    if (!only && std::find(ports.begin(), ports.end(), p) == ports.end())
      ports.push_back(p);

  for (const int p : ports) {
    const std::string url = "https://" + host + (p == 443 ? "" : ":" + std::to_string(p));
    bool reached = false;
    const std::string version = openhab_at(url, appcfg::Tls::Insecure, reached);
    if (version.empty())
      continue;   // nothing there, no https, or not openHAB
    d.found = true;
    d.port = p;
    d.version = version == "?" ? "" : version;
    bool vouched = false;
    openhab_at(url, appcfg::Tls::Bundle, vouched);
    if (vouched) {   // the built-in CAs know it (and its name): nothing to import
      d.public_ca = true;
      d.name_ok = true;
      break;
    }
    const auto chain = chain_of(host, p);
    if (chain.empty() || chain[0].empty())
      break;
    mbedtls_x509_crt leaf, top;
    mbedtls_x509_crt_init(&leaf);
    mbedtls_x509_crt_init(&top);
    const std::vector<uint8_t> &last = chain.back();
    if (mbedtls_x509_crt_parse_der(&leaf, chain[0].data(), chain[0].size()) == 0 &&
        mbedtls_x509_crt_parse_der(&top, last.data(), last.size()) == 0) {
      d.names = names_of(leaf);
      d.name_ok = std::any_of(d.names.begin(), d.names.end(), [&](const std::string &n) { return name_covers(n, host); });
      // only a chain ending in a self-signed certificate gives the panel something to trust
      d.importable = top.subject_raw.len == top.issuer_raw.len &&
                     memcmp(top.subject_raw.p, top.issuer_raw.p, top.subject_raw.len) == 0;
      char dn[200] = "";
      mbedtls_x509_dn_gets(dn, sizeof(dn), &top.subject);
      d.subject = dn;
      d.self_signed = d.importable && chain.size() == 1;
      char until[16];
      snprintf(until, sizeof(until), "%04d-%02d-%02d", top.valid_to.year, top.valid_to.mon, top.valid_to.day);
      d.valid_to = until;
      if (d.importable) {
        d.pem = base64_pem(last);
        d.fingerprint = fingerprint(last);
      }
    }
    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&top);
    break;
  }
  ESP_LOGI(TAG, "https on %s: %s", host.c_str(),
           !d.found ? "none" : d.public_ca ? "public certificate" : d.importable ? "certificate to trust" : "unknown CA");
  return d;
}

}  // namespace appoh
