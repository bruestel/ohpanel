/*
   WiFi with an access-point fallback.

   The panel sits in a wall box where no cable can be attached, so it must
   always remain reachable somehow. If a station connection is not
   configured or cannot be established, it opens its own network with the
   setup page at 192.168.4.1, which is the only way back in.
*/

#pragma once

#include "appcfg/Config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace appnet {

enum class State : uint8_t {
  Idle,
  Connecting,
  Connected,
  ApMode,  // own network, the setup page at 192.168.4.1
};

const char *to_string(State s);

using StateCallback = std::function<void(State)>;

bool begin(StateCallback on_state_change);

State state();
std::string ip();
std::string ssid();
int8_t rssi();

/* What the joined network offers ("open", "wpa2", "wpa3", "wpa2/wpa3"), ""
   while not joined. */
const char *security();

/* The channel the station is using. Both radios share one, so this is what
   decides whether a setup access point can stay up alongside it. */
uint8_t channel();

/* True while the device is only reachable through its own access point. Setup
   endpoints must stay open in that case -- requiring a password here would lock
   the user out of a device they cannot reach any other way. */
bool provisioning();

/* Opens the setup hotspot now, whatever the home network does (the right
   button held, src/recovery.cpp): the panel leaves its network and tries it
   again after 10 minutes; joined, the hotspot closes as it always does. */
void open_hotspot();

/* Closes a hotspot open_hotspot opened (any other stays: it is the way in)
   and joins the home network at once. Returns at once. */
void close_hotspot();

/* The hotspot is open because open_hotspot asked for it. */
bool hotspot_by_hand();

/* The station's address as it is now: from DHCP or fixed (appcfg::Ip), the
   netmask's prefix, the gateway, the DNS servers -- empty while not joined. */
struct Address {
  bool fixed = false;      // a fixed address is in use
  bool fallback = false;   // a fixed one is set, but its gateway did not answer: DHCP for now (until a restart)
  std::string ip, gateway, dns1, dns2;
  std::string dns3;        // the third place: an IPv6 one behind two IPv4 ones (tick)
  int prefix = 0;
};
Address address();

/* The station's IPv6 addresses (with appcfg::Ip::ipv6): link-local, and what SLAAC made of the router's
   prefixes. `kind`: "link-local", "unique-local" (fc00::/7) or "global". */
struct Address6 {
  std::string address;
  const char *kind;
  uint8_t bytes[16];
};
std::vector<Address6> ipv6();

/* Called (from the event task) when an address came or changed while joined: an IPv6 one, or the IPv4 one
   after the network was joined by IPv6. Joining itself is the state callback's (begin). */
void on_address(std::function<void()> cb);

/* The IPv6 router (link-local, from its announcements), "" if none: the gateway of an IPv6 network. */
std::string ipv6_router();

/* The time servers SNTP asks, in order, and where each came from: "dhcp" (the router's, DHCP option 42),
   "configured" (Settings > Device) or "pool" (pool.ntp.org, the fallback). */
struct TimeServer {
  std::string server;
  const char *source;
};
std::vector<TimeServer> time_servers();

/* Now and then from the main loop: DHCPv6 puts its DNS servers in front of the IPv4 ones (the same slots in
   lwIP); they go behind them again -- an IPv4 network that names a DNS server keeps it, IPv6 is the reserve. */
void tick();

/* A fixed address whose gateway never answered (src/netwatch.cpp): DHCP for
   now, the setting kept; a restart tries it again. */
void use_dhcp_for_now();

/* Whether a fixed address makes sense: four numbers each, the prefix 8 to
   30, the address no network or broadcast address and the gateway in its
   network. `error` says what is wrong. */
bool valid_ip(const appcfg::Ip &ip, std::string *error);

/* A network seen by a scan, strongest access point per SSID. */
struct Network {
  std::string ssid;
  int8_t rssi;
  bool secured;
  /* "open", "wpa2", "wpa3", "wpa2/wpa3" -- what the panel joins -- or
     "unsupported" (WEP, WPA without WPA2, enterprise). */
  const char *security;
};
/* Blocking scan (a few seconds). Fails while the station is joining. */
bool scan(std::vector<Network> &out);

/* Wall-clock time, once SNTP has managed to set it.

   The clock runs in UTC. `boot_epoch_ms` is the epoch time at which the uptime
   clock read zero, so uptime stamps can be turned into absolute times. It is
   derived on every call, so a later NTP correction applies retroactively. */
struct Time {
  bool synced = false;
  int64_t epoch_ms = 0;       // now, milliseconds since the Unix epoch, UTC
  int64_t boot_epoch_ms = 0;  // epoch time at which the uptime clock read zero
};
Time time_info();

/* Takes the time server and timezone from the settings (appcfg::clock): sets
   TZ, and restarts SNTP if it runs with another server. Call at boot, before
   anything formats a time, and after changing them. */
void apply_clock();

}  // namespace appnet
