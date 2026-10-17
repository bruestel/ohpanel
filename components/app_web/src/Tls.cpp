/* The panel's CA and its server certificate (Tls.h). */

#include "Tls.h"

#include <lwip/sockets.h>
#include <esp_log.h>
#include <mbedtls/oid.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace appweb::tls {
namespace {

const char *const TAG = "tls";
const char *const kNs = "tls";
// valid whatever the panel's clock says: the browser checks the dates, against its own
const char *const kNotBefore = "20250101000000", *const kNotAfter = "20491231235959";
constexpr size_t kPemMax = 4096;

std::mutex g_mx;

std::string nvs_get(const char *key) {
  nvs_handle_t h;
  std::string out;
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK)
    return out;
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 1) {
    out.resize(len);
    if (nvs_get_str(h, key, out.data(), &len) == ESP_OK)
      out.resize(len - 1);
    else
      out.clear();
  }
  nvs_close(h);
  return out;
}

bool nvs_put(const std::vector<std::pair<const char *, std::string>> &values) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok = true;
  for (const auto &[key, value] : values)
    ok = ok && nvs_set_str(h, key, value.c_str()) == ESP_OK;
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

/* A new ECDSA P-256 key, as PEM. */
bool new_key(std::string &pem) {
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&a, 256);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
  psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  mbedtls_svc_key_id_t id;
  if (psa_generate_key(&a, &id) != PSA_SUCCESS)
    return false;
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  std::vector<unsigned char> buf(kPemMax);
  const bool ok = mbedtls_pk_copy_from_psa(id, &pk) == 0 && mbedtls_pk_write_key_pem(&pk, buf.data(), buf.size()) == 0;
  psa_destroy_key(id);
  mbedtls_pk_free(&pk);
  if (ok)
    pem = reinterpret_cast<const char *>(buf.data());
  return ok;
}

bool parse_key(const std::string &pem, mbedtls_pk_context &pk) {
  return mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1, nullptr, 0) == 0;
}

/* DER: a tag, its length, its content. */
std::string tlv(uint8_t tag, const std::string &content) {
  std::string out(1, static_cast<char>(tag));
  const size_t n = content.size();
  if (n < 0x80) {
    out += static_cast<char>(n);
  } else if (n < 0x100) {
    out += '\x81';
    out += static_cast<char>(n);
  } else {
    out += '\x82';
    out += static_cast<char>(n >> 8);
    out += static_cast<char>(n & 0xFF);
  }
  return out + content;
}

/* The CA's name constraints (RFC 5280 4.2.1.10): what it may vouch for -- names
   under .local and the suffixes home routers hand out, private and link-local
   addresses, IPv4 and IPv6. Nothing on the internet. */
/* "2001:db8:1:2::/64" (or an IPv4 "a.b.c.d/n"): the address and its mask, as a name constraint's iPAddress holds
   them -- 8 or 32 bytes. False if it is no such prefix. */
bool parse_prefix(const std::string &s, std::string &addr_mask) {
  const size_t slash = s.find('/');
  if (slash == std::string::npos || slash + 1 >= s.size())
    return false;
  const std::string addr = s.substr(0, slash);
  char *end = nullptr;
  const long bits = std::strtol(s.c_str() + slash + 1, &end, 10);
  if (*end)
    return false;
  uint8_t a[16];
  size_t n;
  if (inet_pton(AF_INET6, addr.c_str(), a) == 1)
    n = 16;
  else if (inet_pton(AF_INET, addr.c_str(), a) == 1)
    n = 4;
  else
    return false;
  // a prefix of the home network, not a whole provider: /32 to /64 (IPv6), /8 to /32 (IPv4)
  if (n == 16 ? (bits < 32 || bits > 64) : (bits < 8 || bits > 32))
    return false;
  std::string mask(n, '\0');
  for (size_t i = 0; i < n; i++) {
    const long b = std::clamp(bits - static_cast<long>(i) * 8, 0L, 8L);
    mask[i] = static_cast<char>(b ? 0xFF << (8 - b) : 0);
    a[i] &= static_cast<uint8_t>(mask[i]);
  }
  addr_mask = std::string(reinterpret_cast<char *>(a), n) + mask;
  return true;
}

std::string name_constraints(const std::vector<std::string> &extra) {
  std::string subtrees;
  for (const char *dns : {"local", "lan", "home", "home.arpa", "internal", "localdomain", "fritz.box"})
    subtrees += tlv(0x30, tlv(0x82, dns));   // GeneralSubtree { base dNSName }
  for (const std::string &e : extra) {   // the names the user added (they and the ones under them), and prefixes
    std::string addr_mask;
    subtrees += parse_prefix(e, addr_mask) ? tlv(0x30, tlv(0x87, addr_mask)) : tlv(0x30, tlv(0x82, e));
  }
  const auto ip = [&](std::initializer_list<uint8_t> addr_mask) {
    subtrees += tlv(0x30, tlv(0x87, std::string(addr_mask.begin(), addr_mask.end())));   // base iPAddress
  };
  ip({10, 0, 0, 0, 255, 0, 0, 0});            // 10.0.0.0/8
  ip({172, 16, 0, 0, 255, 240, 0, 0});        // 172.16.0.0/12
  ip({192, 168, 0, 0, 255, 255, 0, 0});       // 192.168.0.0/16
  ip({169, 254, 0, 0, 255, 255, 0, 0});       // link-local
  ip({0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // fc00::/7, unique local
      0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  ip({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,       // fe80::/10, link-local
      0xff, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  return tlv(0x30, tlv(0xA0, subtrees));   // NameConstraints { permittedSubtrees [0] }
}

std::vector<std::string> split(const std::string &list) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < list.size()) {
    const size_t end = std::min(list.find(',', at), list.size());
    if (end > at)
      out.push_back(list.substr(at, end - at));
    at = end + 1;
  }
  return out;
}

/* Writes a certificate for `subject_key` signed with `issuer_key` -- the CA's own (self-signed) when `ca`. */
bool issue(bool ca, mbedtls_pk_context &subject_key, mbedtls_pk_context &issuer_key, const std::string &subject,
           const std::string &issuer, const Names *names, std::string &pem) {
  mbedtls_x509write_cert c;
  mbedtls_x509write_crt_init(&c);
  mbedtls_x509write_crt_set_version(&c, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&c, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&c, &subject_key);
  mbedtls_x509write_crt_set_issuer_key(&c, &issuer_key);
  unsigned char serial[16];
  psa_generate_random(serial, sizeof(serial));
  serial[0] &= 0x7F;   // positive
  serial[0] |= 0x01;   // and no leading zero byte
  int r = mbedtls_x509write_crt_set_subject_name(&c, subject.c_str());
  r = r ? r : mbedtls_x509write_crt_set_issuer_name(&c, issuer.c_str());
  r = r ? r : mbedtls_x509write_crt_set_serial_raw(&c, serial, sizeof(serial));
  r = r ? r : mbedtls_x509write_crt_set_validity(&c, kNotBefore, kNotAfter);
  r = r ? r : mbedtls_x509write_crt_set_basic_constraints(&c, ca ? 1 : 0, ca ? 0 : -1);
  r = r ? r : mbedtls_x509write_crt_set_subject_key_identifier(&c);
  r = r ? r : mbedtls_x509write_crt_set_authority_key_identifier(&c);
  std::vector<mbedtls_x509_san_list> san;
  mbedtls_asn1_sequence server_auth = {};
  std::string constraints;
  if (ca) {
    r = r ? r : mbedtls_x509write_crt_set_key_usage(&c, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
    constraints = name_constraints(split(nvs_get("hosts")));
    r = r ? r : mbedtls_x509write_crt_set_extension(&c, MBEDTLS_OID_NAME_CONSTRAINTS,
                                                    MBEDTLS_OID_SIZE(MBEDTLS_OID_NAME_CONSTRAINTS), 1,
                                                    reinterpret_cast<const unsigned char *>(constraints.data()),
                                                    constraints.size());
  } else {
    r = r ? r : mbedtls_x509write_crt_set_key_usage(&c, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    server_auth.buf.tag = MBEDTLS_ASN1_OID;
    server_auth.buf.p = reinterpret_cast<unsigned char *>(const_cast<char *>(MBEDTLS_OID_SERVER_AUTH));
    server_auth.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    r = r ? r : mbedtls_x509write_crt_set_ext_key_usage(&c, &server_auth);
    san.resize(names->dns.size() + names->ips.size());
    size_t k = 0;
    for (const std::string &d : names->dns) {
      san[k].node.type = MBEDTLS_X509_SAN_DNS_NAME;
      san[k].node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(d.data()));
      san[k].node.san.unstructured_name.len = d.size();
      k++;
    }
    for (const auto &ip : names->ips) {
      san[k].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
      san[k].node.san.unstructured_name.p = const_cast<unsigned char *>(ip.data());
      san[k].node.san.unstructured_name.len = ip.size();
      k++;
    }
    for (size_t i = 0; i + 1 < san.size(); i++)
      san[i].next = &san[i + 1];
    if (!san.empty())
      r = r ? r : mbedtls_x509write_crt_set_subject_alternative_name(&c, san.data());
  }
  std::vector<unsigned char> buf(kPemMax);
  r = r ? r : mbedtls_x509write_crt_pem(&c, buf.data(), buf.size());
  mbedtls_x509write_crt_free(&c);
  if (r != 0) {
    ESP_LOGE(TAG, "%s certificate: error -0x%04x", ca ? "CA" : "server", static_cast<unsigned>(-r));
    return false;
  }
  pem = reinterpret_cast<const char *>(buf.data());
  return true;
}

/* What the server certificate is for, as a string to compare: a new one when it changes. */
std::string names_key(const Names &n) {
  std::string out;
  for (const auto &d : n.dns)
    out += d + ";";
  for (const auto &ip : n.ips) {
    char hex[3];
    for (uint8_t b : ip) {
      snprintf(hex, sizeof(hex), "%02x", b);
      out += hex;
    }
    out += ";";
  }
  return out;
}

std::string device_name(const Names &n) {
  const std::string first = n.dns.empty() ? "ohpanel" : n.dns.front();
  return first.substr(0, first.find('.'));
}

std::string g_last;   // the chain last handed out: what "changed" compares with

/* mbedTLS does not know the name constraints the CA carries, marked critical: read anyway -- only to describe it
   (browsers check them; the panel never verifies a chain with it). */
int any_extension(void *, const mbedtls_x509_crt *, const mbedtls_x509_buf *, int, const unsigned char *,
                  const unsigned char *) {
  return 0;
}

/* The first certificate of a PEM, read with extensions mbedTLS does not know (see any_extension). */
bool parse_cert(const std::string &pem, mbedtls_x509_crt &crt) {
  mbedtls_pem_context p;
  mbedtls_pem_init(&p);
  size_t used = 0;
  const bool ok = !pem.empty() &&
                  mbedtls_pem_read_buffer(&p, "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----",
                                          reinterpret_cast<const unsigned char *>(pem.c_str()), nullptr, 0, &used) == 0 &&
                  mbedtls_x509_crt_parse_der_with_ext_cb(&crt, p.MBEDTLS_PRIVATE(buf), p.MBEDTLS_PRIVATE(buflen), 1,
                                                         any_extension, nullptr) == 0;
  mbedtls_pem_free(&p);
  return ok;
}

/* Its SHA-256 fingerprint ("AB:CD:..."), its names and until when it is valid. */
void describe(const std::string &pem, std::string &fingerprint, std::string &valid_to, std::string *subject,
              std::vector<std::string> *names) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  if (parse_cert(pem, crt)) {
    uint8_t hash[32];
    size_t len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len, hash, sizeof(hash), &len) == PSA_SUCCESS) {
      char hex[4];
      for (size_t i = 0; i < len; i++) {
        snprintf(hex, sizeof(hex), i ? ":%02X" : "%02X", hash[i]);
        fingerprint += hex;
      }
    }
    char until[16];
    snprintf(until, sizeof(until), "%04d-%02d-%02d", crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
    valid_to = until;
    if (subject) {
      char dn[200] = "";
      mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject);
      // mbedTLS writes the bytes of a UTF-8 letter as "\C3\BC" (RFC 4514): back to the letter, to show
      for (const char *s = dn; *s; s++) {
        if (s[0] == '\\' && isxdigit(static_cast<unsigned char>(s[1])) && isxdigit(static_cast<unsigned char>(s[2]))) {
          *subject += static_cast<char>(std::strtol(std::string(s + 1, 2).c_str(), nullptr, 16));
          s += 2;
        } else {
          *subject += *s;
        }
      }
    }
    if (names) {
      for (const mbedtls_x509_sequence *s = &crt.subject_alt_names; s && s->buf.p; s = s->next) {
        mbedtls_x509_subject_alternative_name san;
        if (mbedtls_x509_parse_subject_alt_name(&s->buf, &san) != 0)
          continue;
        const mbedtls_x509_buf &b = san.san.unstructured_name;
        if (san.type == MBEDTLS_X509_SAN_DNS_NAME) {
          names->emplace_back(reinterpret_cast<const char *>(b.p), b.len);
        } else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && b.len == 4) {
          char ip[16];
          snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b.p[0], b.p[1], b.p[2], b.p[3]);
          names->emplace_back(ip);
        } else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && b.len == 16) {
          char ip[48];
          inet_ntop(AF_INET6, b.p, ip, sizeof(ip));
          names->emplace_back(ip);
        }
        mbedtls_x509_free_subject_alt_name(&san);
      }
    }
  }
  mbedtls_x509_crt_free(&crt);
}

}  // namespace

bool ensure(const Names &given, std::string &chain_pem, std::string &key_pem, bool *changed) {
  std::lock_guard<std::mutex> lock(g_mx);
  psa_crypto_init();
  Names names = given;   // and the names the user added
  for (const std::string &h : split(nvs_get("hosts")))
    if (h.find('/') == std::string::npos)   // a prefix: the addresses in it come with `given` (covered())
      names.dns.push_back(h);
  std::string ca_key = nvs_get("ca_key"), ca_crt = nvs_get("ca_crt");
  const std::string name = device_name(names);
  if ((ca_key.empty() || ca_crt.empty()) && nvs_get("ca_own") != "1") {   // the first start: the CA
    ESP_LOGI(TAG, "Making the panel's certificate authority");
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const std::string dn = "CN=OHpanel " + name + " CA,O=OHpanel";
    // (its key kept when only its certificate goes: other names it may vouch for)
    const bool ok = (ca_key.empty() ? new_key(ca_key) : true) && parse_key(ca_key, pk) &&
                    issue(true, pk, pk, dn, dn, nullptr, ca_crt);
    mbedtls_pk_free(&pk);
    // its name kept as written: an issuer's must match it byte for byte, after a rename of the panel too
    if (!ok || !nvs_put({{"ca_key", ca_key}, {"ca_crt", ca_crt}, {"ca_dn", dn}, {"srv_names", ""}}))
      return false;
  }
  std::string srv_key = nvs_get("srv_key"), srv_crt = nvs_get("srv_crt");
  const std::string want = names_key(names);
  if (srv_key.empty() || srv_crt.empty() || nvs_get("srv_names") != want) {   // the names it goes by changed
    ESP_LOGI(TAG, "Issuing the server certificate for %s", want.c_str());
    mbedtls_pk_context key, ca;
    mbedtls_pk_init(&key);
    mbedtls_pk_init(&ca);
    const std::string issuer = nvs_get("ca_dn");   // as the CA certificate has it
    const std::string subject = "CN=" + (names.dns.empty() ? name : names.dns.front()) + ",O=OHpanel";
    const bool ok = !issuer.empty() && (srv_key.empty() ? new_key(srv_key) : true) && parse_key(srv_key, key) &&
                    parse_key(ca_key, ca) && issue(false, key, ca, subject, issuer, &names, srv_crt);
    mbedtls_pk_free(&key);
    mbedtls_pk_free(&ca);
    if (!ok || !nvs_put({{"srv_key", srv_key}, {"srv_crt", srv_crt}, {"srv_names", want}}))
      return false;
  }
  chain_pem = srv_crt + ca_crt;   // the server's, then the CA's: a browser that knows the CA needs nothing else
  key_pem = srv_key;
  if (changed)
    *changed = chain_pem != g_last;
  g_last = chain_pem;
  return true;
}

bool force_https() {
  std::lock_guard<std::mutex> lock(g_mx);
  return nvs_get("force") == "1";
}

bool set_force_https(bool on) {
  std::lock_guard<std::mutex> lock(g_mx);
  return nvs_put({{"force", on ? "1" : "0"}});
}

bool renew() {
  std::lock_guard<std::mutex> lock(g_mx);
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK)
    return false;
  for (const char *key : {"ca_key", "ca_crt", "ca_dn", "ca_own", "srv_key", "srv_crt", "srv_names"})
    nvs_erase_key(h, key);
  const bool ok = nvs_commit(h) == ESP_OK;
  nvs_close(h);
  ESP_LOGW(TAG, "Keys erased: a new CA comes");
  return ok;
}

bool set_ca(const std::string &cert_pem, const std::string &key_pem, std::string *error) {
  const auto fail = [error](const char *why) {
    if (error)
      *error = why;
    return false;
  };
  if (cert_pem.size() > kPemMax || key_pem.size() > kPemMax)
    return fail("certificate or key too long");
  std::lock_guard<std::mutex> lock(g_mx);
  psa_crypto_init();
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  std::string subject;
  const char *why = nullptr;
  if (!parse_cert(cert_pem, crt))
    why = "not a certificate (PEM)";
  else if (!mbedtls_x509_crt_get_ca_istrue(&crt))
    why = "not a CA certificate (no basic constraints CA:TRUE)";
  else if (!parse_key(key_pem, key))
    why = "not a private key (PEM, unencrypted)";
  else if (mbedtls_pk_check_pair(&crt.pk, &key) != 0)
    why = "the key does not belong to the certificate";
  if (!why) {   // "CN=a, O=b" as mbedTLS writes it, back to what it reads: "CN=a,O=b"
    char buf[256] = "";
    mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.subject);
    for (const char *s = buf; *s; s++) {
      if (s[0] == ',' && s[1] == ' ' && s > buf && s[-1] != '\\') {
        subject += ',';
        s++;
      } else {
        subject += *s;
      }
    }
  }
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&crt);
  if (why)
    return fail(why);
  // the server certificate issued anew by it (its key kept); renew() goes back to a CA of the panel's own
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK)
    return fail("could not store");
  bool ok = nvs_set_str(h, "ca_crt", cert_pem.c_str()) == ESP_OK && nvs_set_str(h, "ca_key", key_pem.c_str()) == ESP_OK &&
            nvs_set_str(h, "ca_dn", subject.c_str()) == ESP_OK && nvs_set_str(h, "ca_own", "1") == ESP_OK;
  for (const char *k : {"srv_crt", "srv_names"})
    nvs_erase_key(h, k);
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  ESP_LOGW(TAG, "An own CA set: %s", subject.c_str());
  return ok || fail("could not store");
}

std::vector<std::string> extra_hosts() {
  std::lock_guard<std::mutex> lock(g_mx);
  return split(nvs_get("hosts"));
}

bool covered(const uint8_t *ip, size_t len) {
  for (const std::string &e : extra_hosts()) {
    std::string am;
    if (!parse_prefix(e, am) || am.size() != len * 2)
      continue;
    bool in = true;
    for (size_t i = 0; i < len && in; i++)
      in = (ip[i] & static_cast<uint8_t>(am[len + i])) == static_cast<uint8_t>(am[i]);
    if (in)
      return true;
  }
  return false;
}

bool set_extra_hosts(const std::vector<std::string> &hosts, std::string *error) {
  std::string list;
  for (std::string h : hosts) {
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    if (std::string am; h.find('/') != std::string::npos) {
      if (!parse_prefix(h, am)) {
        if (error)
          *error = "not a prefix (IPv6 /32 to /64, IPv4 /8 to /32): " + h;
        return false;
      }
      list += (list.empty() ? "" : ",") + h;
      continue;
    }
    bool ok = !h.empty() && h.size() <= 100 && h.front() != '.' && h.front() != '-' && h.back() != '.';
    for (const char c : h)
      ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-');
    if (!ok) {
      if (error)
        *error = "not a host name: " + h;
      return false;
    }
    list += (list.empty() ? "" : ",") + h;
  }
  std::lock_guard<std::mutex> lock(g_mx);
  if (nvs_get("hosts") == list)
    return true;
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK)
    return false;
  // a new CA certificate (its key kept) and a new server certificate: the next ensure() makes them -- an own CA's
  // certificate stays as it was given (the server certificate takes the new names)
  bool ok = nvs_set_str(h, "hosts", list.c_str()) == ESP_OK;
  const bool own = nvs_get("ca_own") == "1";
  for (const char *key : {"ca_crt", "srv_crt", "srv_names"})
    if (!own || std::strcmp(key, "ca_crt") != 0)
      nvs_erase_key(h, key);
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

std::string ca_pem() {
  std::lock_guard<std::mutex> lock(g_mx);
  return nvs_get("ca_crt");
}

Info info() {
  std::lock_guard<std::mutex> lock(g_mx);
  Info i;
  describe(nvs_get("ca_crt"), i.ca_fingerprint, i.ca_valid_to, &i.ca_subject, nullptr);
  i.ca_own = nvs_get("ca_own") == "1";
  describe(nvs_get("srv_crt"), i.server_fingerprint, i.server_valid_to, nullptr, &i.server_names);
  return i;
}

}  // namespace appweb::tls
