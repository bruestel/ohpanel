/*
   The panel's own certificate authority and the certificate its web interface
   answers https with (Tls.cpp).

   Made on the first start: an ECDSA P-256 key and a CA certificate for it,
   then a server certificate for the names the panel goes by -- issued again
   whenever those change (another IP address, another name), by the same CA,
   so a browser that trusts it keeps trusting the panel.

   Both are valid from 2025 to 2049, whatever the panel's clock says (it may
   have none yet: in its own setup network): it is the browser that checks a
   certificate's dates, against its own clock. The CA is restricted (name
   constraints) to names under .local and a few home-network suffixes and to
   private addresses, IPv4 and IPv6: trusting it lets it vouch for the
   panel, not for any site on the internet -- should its key ever leave the
   panel.

   Kept in NVS (namespace "tls"), not in the documents of the storage
   partition: nothing that lists or backs those up carries the CA's key.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace appweb::tls {

/* What a certificate is issued for: names (DNS) and addresses (4 or 16
   bytes). */
struct Names {
  std::vector<std::string> dns;
  std::vector<std::vector<uint8_t>> ips;
};

/* The certificate chain (the server's, then the CA's) and the server's key,
   both PEM, for `names`: the stored ones while they fit, else a new server
   certificate -- and the CA made first if there is none. `changed`: they
   differ from the last ones returned. False if they could not be made. */
bool ensure(const Names &names, std::string &chain_pem, std::string &key_pem, bool *changed = nullptr);

/* The CA's certificate (PEM), for a browser to install; empty if none. */
std::string ca_pem();

/* What the web interface shows of them. */
struct Info {
  std::string ca_subject, ca_fingerprint, ca_valid_to;
  bool ca_own = false;   // set with set_ca
  std::string server_fingerprint, server_valid_to;
  std::vector<std::string> server_names;
};
Info info();

/* "Always use https": the interface's page asked for over http is sent to https (Web.cpp) -- not in the setup
   network, whose page a phone opens over http by itself. Off unless set. */
bool force_https();
bool set_force_https(bool on);

/* New keys: a new CA and a new server certificate (every browser must trust the new CA). False if they could not
   be stored; the next ensure() makes them. An own CA (set_ca) goes too: the panel makes its own again. */
bool renew();

/* An own CA instead of the one the panel made: its certificate and its private key (PEM, any key mbedTLS reads),
   e.g. one CA for every panel of a house, trusted once. Checked: a CA certificate, the key its own. The server
   certificate is issued by it from then on (the next ensure()). Its key never leaves the panel again (no backup
   carries it). False with `error` if it is refused or cannot be stored. */
bool set_ca(const std::string &cert_pem, const std::string &key_pem, std::string *error);

/* More names the panel goes by (a name of its own in the home network's DNS, say): the CA may vouch for them too
   (and the names under them), the server certificate is issued for them. Changing them makes a new CA certificate,
   with the same key: every browser must trust it anew. Lower case letters, digits, dots and dashes.
   Also prefixes ("2001:db8:1:2::/64"; IPv6 /32 to /64, IPv4 /8 to /32): addresses in them the CA may vouch for,
   e.g. a global IPv6 prefix of a network whose provider keeps it. */
std::vector<std::string> extra_hosts();
bool set_extra_hosts(const std::vector<std::string> &hosts, std::string *error);

/* Whether an address (4 or 16 bytes) is in one of the prefixes above: then it goes into the server certificate. */
bool covered(const uint8_t *ip, size_t len);

}  // namespace appweb::tls
