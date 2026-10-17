/*
   Persistent configuration in NVS.

   Everything the user can change at runtime lives here, so the firmware never
   has to be rebuilt to move to a different network or change a password.
*/

#pragma once

#include <cstdint>
#include <string>

namespace appcfg {

struct Wifi {
  std::string ssid;
  std::string password;
  /* Of a WPA2/WPA3 network only WPA3, no falling back to WPA2. Off: the best
     both offer, WPA2 at least (with a password). */
  bool wpa3_only = false;
  bool configured() const { return !ssid.empty(); }
};

/* The station's address: from DHCP (default), or fixed -- the address, its
   prefix (/24: 255.255.255.0), the gateway, one or two DNS servers. */
struct Ip {
  bool fixed = false;
  std::string address, gateway, dns1, dns2;
  int prefix = 24;
  bool ipv6 = true;   // IPv6 too: addresses by SLAAC, DNS servers by DHCPv6
};

struct Auth {
  /* Off by default: a fresh device must be reachable to be set up at all. The
     user turns it on and picks the password. */
  bool enabled = false;
  std::string user = "admin";
  std::string salt_hex;
  std::string hash_hex;
  bool has_password() const { return !hash_hex.empty(); }
};

/* The openHAB instance the panel reads item states from and sends commands to,
   through its REST API.

   The token is an openHAB API token ("oh.<name>.<secret>"), ideally of a
   dedicated account with the user role. Empty means anonymous access, which
   openHAB allows by default ("implicit user role") but which stops working the
   moment that setting is turned off. */
/* How an https:// connection checks openHAB's certificate: against the
   public CAs of the built-in bundle, against the CA given here (a private
   CA, or a self-signed certificate itself), or not at all. */
enum class Tls : uint8_t { Bundle, Custom, Insecure };

struct OpenHab {
  std::string url;    // e.g. "http://openhab.local:8080", no trailing slash
  std::string token;
  Tls tls = Tls::Bundle;
  std::string ca;     // PEM, for Tls::Custom
  bool check_name = true;  // the certificate must name the host of the URL (Bundle, Custom)
  bool configured() const { return !url.empty(); }
};

constexpr size_t kMaxCaPem = 6144;

struct Device {
  /* Also used as the DHCP and mDNS hostname and the hotspot SSID. */
  std::string name;
};

/* Time server and timezone.

   The clock runs in UTC; the timezone only decides how the display and the
   web interface show it. It is stored twice: the IANA name for the browser
   and the web interface, the POSIX rule for the C library (TZ), which knows no
   names. */
struct Clock {
  std::string server;  // empty: the router's (DHCP), then the built-in pool
  std::string tz_name = "Europe/Berlin";
  std::string tz_posix = "CET-1CEST,M3.5.0,M10.5.0/3";
};

/* Initialises NVS. */
bool begin();

Wifi wifi();
bool set_wifi(const std::string &ssid, const std::string &password, bool wpa3_only = false);

Ip ip();
bool set_ip(const Ip &ip);

Auth auth();
/* Stores a PBKDF2-HMAC-SHA256 digest with a fresh random salt. An empty
   password clears the stored credential and forces protection off. */
bool set_password(const std::string &user, const std::string &password);
bool set_auth_enabled(bool enabled);
bool set_auth_user(const std::string &user);  // the password stays (its hash does not involve the user)
/* The password as a backup has it: salt and hash (hex), as stored; false if
   they are not of the stored kind. */
bool set_auth_hash(const std::string &user, const std::string &salt_hex, const std::string &hash_hex);
bool verify_password(const std::string &user, const std::string &password);

OpenHab openhab();
bool set_openhab(const OpenHab &o);

Device device();
bool set_device_name(const std::string &name);

Clock clock();
bool set_clock(const Clock &c);

/* The panel's temperature sensor (an NTC next to the board). The panel warms
   itself, so it reads high; the offset corrects that, in tenths of a degree
   (-25 = 2.5 degrees lower). With an item, the corrected value is reported to
   openHAB as that item's state. */
struct Sensor {
  int offset_tenths = 0;
  std::string item;
};

Sensor sensor();
bool set_sensor(const Sensor &s);

/* Last state of the two relays (index 0/1), restored at boot -- a light on a
   relay stays on through a power cut or a firmware update. */
bool relay(int index);
bool set_relay(int index, bool on);

/* WiFi transmit power in dBm. 0 means "use the maximum".

   Not capped by default: reducing power to guard against a brownout nobody has
   observed trades a certain cost for an uncertain benefit. Brownouts are
   detected by name, so the firmware can step this down when one actually
   happens instead of assuming it will. */
int tx_power_dbm();
bool set_tx_power_dbm(int dbm);

/* "ohpanel-a1b2c3" from the WiFi MAC -- the default device name and the name of
   the setup hotspot. */
std::string default_device_name();

/* Erases every setting this component owns: network, credentials, device name.
   Distinct from the BOOT-button hold, which only clears what can lock you out
   and deliberately keeps the rest. */
bool factory_reset();

}  // namespace appcfg
