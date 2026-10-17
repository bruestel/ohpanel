/*
   WiFi with an access-point fallback.
*/

#include "appnet/Net.h"
#include "appcfg/Config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <lwip/dhcp6.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/priv/nd6_priv.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ctime>
#include <sys/time.h>

namespace appnet {
namespace {

const char *const TAG = "appnet";

/* Six attempts five seconds apart, so roughly half a minute of patience before
   the setup network appears. Retrying without a pause -- as this did at first --
   burns all attempts in a couple of seconds and rides out nothing at all. */
constexpr int kMaxStaRetries = 6;
constexpr int64_t kStaRetryUs = 5 * 1000000LL;
/* The access point's fixed address: what it reports as the device's address
   while in setup mode. */
constexpr const char *kApAddress = "192.168.4.1";

/* After a successful join the setup network lingers briefly. Whoever just
   entered the credentials is still connected to it and needs to see that it
   worked -- and to read the new address -- before it disappears under them. */
constexpr int64_t kApLingerUs = 120 * 1000000LL;

/* While the setup network is up the station side still tries to get back on the
   real one, just slowly. Without this the device would sit in access-point mode
   forever after a router reboot, and someone would have to go and fetch it. */
constexpr int64_t kApRetryUs = 30 * 1000000LL;

/* A hotspot opened by hand (open_hotspot) leaves the home network alone this
   long before trying it again: time to join the hotspot and change the WiFi. */
constexpr int64_t kByHandUs = 10 * 60 * 1000000LL;

/* While a device is joined to the setup network, it stays as it is -- no
   retrying the home network (joining it moves the radio to the router's
   channel, and the phone drops off), no closing -- up to this long after it
   opened, so a phone left joined cannot keep the panel off its network. */
constexpr int64_t kHoldMaxUs = 60 * 60 * 1000000LL;
constexpr int64_t kHoldCheckUs = 30 * 1000000LL;

State g_state = State::Idle;
StateCallback g_cb;
int g_retries = 0;
esp_netif_t *g_sta_netif = nullptr;
esp_netif_t *g_ap_netif = nullptr;
char g_ip[16] = "";
std::mutex g_ip_mx;   // g_ip: written by the event task, read by the main and the web server tasks
bool g_ap_active = false;
std::atomic<bool> g_by_hand{false};       // the hotspot was opened by hand
std::atomic<int64_t> g_by_hand_until{0};  // ... and the home network waits until then
esp_timer_handle_t g_open_timer = nullptr;
esp_timer_handle_t g_close_timer = nullptr;
esp_timer_handle_t g_ap_linger_timer = nullptr;
int64_t g_ap_opened_at = 0;   // when the setup network opened (kHoldMaxUs)
esp_timer_handle_t g_ap_retry_timer = nullptr;
esp_timer_handle_t g_sta_retry_timer = nullptr;
esp_timer_handle_t g_unpin_timer = nullptr;

bool g_time_synced = false;

void on_time_sync(struct timeval *tv) {
  char buf[32] = "";
  const time_t secs = tv ? tv->tv_sec : 0;
  struct tm utc;
  gmtime_r(&secs, &utc);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc);
  ESP_LOGI(TAG, "Clock %s from NTP: %s UTC", g_time_synced ? "corrected" : "set", buf);
  g_time_synced = true;
}

/* Started once the station has an address, never in AP mode -- there is no
   route to a time server on our own network, and the attempts would just be
   noise in the log.

   Without a server in the settings, the router is asked first via DHCP option
   42, which is both more likely to work on a network without internet access
   and kinder than reaching for a public pool; pool.ntp.org sits behind it for
   routers that do not offer one. Servers are renewed on every new address,
   because a DHCP-supplied server replaces the preconfigured list when it
   arrives. A server from the settings replaces the router's, with the pool
   still behind it, so a wrong or vanished one costs nothing. */
constexpr const char *kPool = "pool.ntp.org";
std::mutex g_sntp_mx;
bool g_sntp_wanted = false;   // the station is up: SNTP should run
bool g_sntp_started = false;
std::string g_sntp_server;    // the configured one SNTP runs with ("" = DHCP)
bool g_fixed = false;         // a fixed address in use (appcfg::Ip): no time server from DHCP either
bool g_fallback = false;      // a fixed one is set, its gateway did not answer: DHCP until a restart
bool g_ipv6 = true;           // appcfg::Ip::ipv6, read at the start
std::function<void()> g_addr_cb;
esp_netif_dns_info_t g_dns4[2] = {};   // the IPv4 DNS servers (DHCP's or the fixed ones): kept in front (tick)

/* DHCP's time server (the first place, as lwIP put it), "" if it named none. */
std::string dhcp_time_server() {
  const ip_addr_t *a = esp_sntp_getserver(0);
  if (esp_sntp_getservername(0) || !a || ip_addr_isany(a))
    return "";
  char buf[48];
  ipaddr_ntoa_r(a, buf, sizeof(buf));
  return buf;
}

/* The pool behind DHCP's server, or in the first place without one. A renewed lease that names a server again
   clears the places behind it (lwIP): put back from tick(). Returns DHCP's server. */
std::string place_pool() {
  const std::string router = dhcp_time_server();
  esp_sntp_setservername(router.empty() ? 0 : 1, kPool);
  return router;
}

void start_sntp_locked() {
  if (g_sntp_started)
    return;
  g_sntp_server = appcfg::clock().server;
  const bool own = !g_sntp_server.empty();

  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(kPool);
  if (own) {
    esp_sntp_config_t two =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(g_sntp_server.c_str(), kPool));
    cfg = two;
  } else if (!g_fixed) {
    /* The router's, from DHCP: lwIP took it when the address came (dhcp mode on since apply_address(), before
       joining) and put it in the first place; the init would write the pool over it -- none given here, the
       pool goes behind it below (place_pool()). */
    cfg.server_from_dhcp = true;
    cfg.num_of_servers = 0;
  }
  cfg.sync_cb = on_time_sync;
  cfg.start = false;   // the servers in place first
  /* Nothing here may block: this runs on the event loop, and the display must
     stay responsive whether or not any time server ever answers. */
  cfg.wait_for_sync = false;

  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SNTP init failed: %s -- timestamps stay relative to boot",
             esp_err_to_name(err));
    return;
  }
  g_sntp_started = true;
  if (own) {
    ESP_LOGI(TAG, "SNTP started: %s, fallback %s", g_sntp_server.c_str(), kPool);
  } else if (g_fixed) {
    ESP_LOGI(TAG, "SNTP started: %s (a fixed address: no time server from DHCP)", kPool);
  } else {
    const std::string router = place_pool();
    if (router.empty())
      ESP_LOGI(TAG, "SNTP started: %s (DHCP named no time server)", kPool);
    else
      ESP_LOGI(TAG, "SNTP started: %s from DHCP, fallback %s", router.c_str(), kPool);
  }
  esp_netif_sntp_start();
}

void start_sntp() {
  std::lock_guard<std::mutex> lock(g_sntp_mx);
  g_sntp_wanted = true;
  start_sntp_locked();
}

void set_state(State s) {
  if (s == g_state)
    return;
  g_state = s;
  ESP_LOGI(TAG, "state: %s", to_string(s));
  if (g_cb)
    g_cb(s);
}


/* Someone on the setup network, and it has not been up too long (kHoldMaxUs). */
bool someone_joined() {
  wifi_sta_list_t list = {};
  return g_ap_active && esp_timer_get_time() - g_ap_opened_at < kHoldMaxUs &&
         esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

void stop_ap() {
  if (!g_ap_active)
    return;
  g_ap_active = false;
  g_by_hand = false;
  if (g_ap_retry_timer)
    esp_timer_stop(g_ap_retry_timer);
  ESP_LOGI(TAG, "Setup network closed; reachable at %s", ip().c_str());
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void start_ap() {
  appcfg::Device dev = appcfg::device();

  if (g_ap_linger_timer)
    esp_timer_stop(g_ap_linger_timer);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  if (!g_ap_active)
    g_ap_opened_at = esp_timer_get_time();
  g_ap_active = true;

  wifi_config_t ap = {};
  const std::string ssid = dev.name;
  std::memcpy(ap.ap.ssid, ssid.data(), std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.ssid_len = static_cast<uint8_t>(std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  /* Set explicitly: left at zero, some phones saw the network appear and vanish
     again because the beacon timing was erratic. */
  ap.ap.beacon_interval = 100;
  /* Open, without a password: it opens when nobody can reach the panel
     otherwise, and a password would have to come from the display -- which may
     run any design, or none. What it guards is the setup page: the web
     interface's own password protection, when on, applies here too. */
  ap.ap.authmode = WIFI_AUTH_OPEN;

  /* Modem sleep makes an access point miss beacons and drop clients; it only
     pays off on the station side, so it is off while the setup network is up. */
  esp_wifi_set_ps(WIFI_PS_NONE);

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_LOGI(TAG, "Access point \"%s\" (open) at %s", ssid.c_str(), kApAddress);
  {
    std::lock_guard<std::mutex> lock(g_ip_mx);
    std::strncpy(g_ip, kApAddress, sizeof(g_ip) - 1);
  }
  set_state(State::ApMode);

  /* Only worth retrying if there is something to retry with. */
  if (appcfg::wifi().configured() && g_ap_retry_timer) {
    esp_timer_stop(g_ap_retry_timer);
    esp_timer_start_periodic(g_ap_retry_timer, kApRetryUs);
    ESP_LOGI(TAG, "Will keep trying \"%s\" every %llds", appcfg::wifi().ssid.c_str(), kApRetryUs / 1000000);
  }
}

uint32_t parse_ip(const std::string &s, bool *ok) {
  unsigned a, b, c, d;
  char tail;
  *ok = std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
  return *ok ? (a << 24 | b << 16 | c << 8 | d) : 0;   // host order
}

/* The station's address as set: fixed (DHCP off, address, mask, gateway, DNS) or from DHCP. Before joining. */
void apply_address() {
  const appcfg::Ip i = appcfg::ip();
  std::string error;
  g_fixed = i.fixed && !g_fallback && valid_ip(i, &error);
  if (i.fixed && !g_fixed && !g_fallback)
    ESP_LOGW(TAG, "Fixed address not used: %s", error.c_str());
  if (!g_fixed) {
    if (appcfg::clock().server.empty())
      esp_sntp_servermode_dhcp(true);   // before the lease comes: lwIP drops its time server otherwise
    const esp_err_t e = esp_netif_dhcpc_start(g_sta_netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
      ESP_LOGW(TAG, "DHCP client: %s", esp_err_to_name(e));
    return;
  }
  bool ok;
  esp_netif_dhcpc_stop(g_sta_netif);
  esp_netif_ip_info_t info = {};
  info.ip.addr = htonl(parse_ip(i.address, &ok));
  info.gw.addr = htonl(parse_ip(i.gateway, &ok));
  info.netmask.addr = htonl(i.prefix ? 0xFFFFFFFFu << (32 - i.prefix) : 0);
  esp_netif_set_ip_info(g_sta_netif, &info);
  int n = 0;
  for (const std::string &dns : {i.dns1, i.dns2}) {
    const uint32_t a = parse_ip(dns, &ok);
    if (!ok)
      continue;
    esp_netif_dns_info_t d = {};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = htonl(a);
    esp_netif_set_dns_info(g_sta_netif, n++ ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &d);
  }
  ESP_LOGI(TAG, "Fixed address %s/%d, gateway %s, DNS %s", i.address.c_str(), i.prefix, i.gateway.c_str(),
           i.dns1.empty() ? i.gateway.c_str() : i.dns1.c_str());
}

void try_connect() {
  appcfg::Wifi w = appcfg::wifi();
  if (!w.configured()) {
    ESP_LOGI(TAG, "No WiFi credentials stored");
    start_ap();
    return;
  }

  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, w.ssid.data(), std::min(w.ssid.size(), sizeof(sta.sta.ssid)));
  std::memcpy(sta.sta.password, w.password.data(), std::min(w.password.size(), sizeof(sta.sta.password)));
  /* The panel joins open networks, WPA2, WPA3 and WPA2/WPA3 -- not WEP, not
     WPA without WPA2. With a password the weakest it accepts is WPA2 (no open
     access point posing under the same name is joined either), or WPA3 when
     set so (of a WPA2/WPA3 network no falling back to WPA2: protected
     management frames required, as WPA3 has them); without one an open
     network. */
  sta.sta.threshold.authmode = w.password.empty() ? WIFI_AUTH_OPEN
                               : w.wpa3_only     ? WIFI_AUTH_WPA3_PSK
                                                 : WIFI_AUTH_WPA2_PSK;
  if (w.wpa3_only && !w.password.empty())
    sta.sta.pmf_cfg.required = true;
  /* Of several access points with the network's name (a mesh, a repeater) the strongest: every channel is scanned
     before choosing -- a second or two longer than stopping at the first that matches, which on channel 1 may be
     the one at the other end of the house --, and one that refuses the panel twice gives way to the next. */
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta.sta.failure_retry_cnt = 2;
  /* Roaming while connected, where the network leads it: 802.11k (radio measurements: the access point learns how
     well the panel hears its neighbours) and 802.11v (BSS transition: it may send the panel to a better one, which
     the WiFi driver then joins). A network without either simply never asks. */
  sta.sta.rm_enabled = 1;
  sta.sta.btm_enabled = 1;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  apply_address();
  g_retries = 0;
  set_state(State::Connecting);
  esp_wifi_connect();
}

/* Joined the network: an IPv4 address (DHCP's or the fixed one), or -- without one -- a usable IPv6 address
   (unique local or global). */
void on_joined() {
  g_retries = 0;
  if (g_ap_retry_timer)
    esp_timer_stop(g_ap_retry_timer);
  if (g_sta_retry_timer)
    esp_timer_stop(g_sta_retry_timer);
  set_state(State::Connected);
  start_sntp();

  /* Advertise only once we are on the real network -- announcing a name on
     our own access point would just point back at ourselves. */
  static bool mdns_up = false;
  if (!mdns_up) {
    const std::string host = appcfg::device().name;
    if (mdns_init() == ESP_OK) {
      mdns_hostname_set(host.c_str());
      mdns_instance_name_set("OHpanel");
      mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
      ESP_LOGI(TAG, "Also reachable at http://%s.local/", host.c_str());
      mdns_up = true;
    } else {
      ESP_LOGW(TAG, "mDNS unavailable; use the IP address");
    }
  }

  if (g_ap_active && g_ap_linger_timer) {
    ESP_LOGI(TAG, "Closing the setup network in %llds", kApLingerUs / 1000000);
    esp_timer_stop(g_ap_linger_timer);
    esp_timer_start_once(g_ap_linger_timer, kApLingerUs);
  }
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    /* In access-point mode the retry timer drives reconnection attempts, so
       reacting to every failure here would only produce a tight loop. */
    if (g_state == State::ApMode)
      return;
    const auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
    if (++g_retries <= kMaxStaRetries) {
      ESP_LOGW(TAG, "Disconnected (reason %d), retry %d/%d in %llds", ev ? ev->reason : 0, g_retries,
               kMaxStaRetries, kStaRetryUs / 1000000);
      set_state(State::Connecting);
      if (g_sta_retry_timer) {
        esp_timer_stop(g_sta_retry_timer);
        esp_timer_start_once(g_sta_retry_timer, kStaRetryUs);
      } else {
        esp_wifi_connect();
      }
    } else {
      ESP_LOGW(TAG, "Could not join \"%s\", opening setup network", appcfg::wifi().ssid.c_str());
      start_ap();
    }
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && g_unpin_timer) {
    esp_timer_stop(g_unpin_timer);
    esp_timer_start_once(g_unpin_timer, 2 * 1000000);   // unpin(): once the connection is complete
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && g_ipv6) {
    /* IPv6: a link-local address, then SLAAC from the router's advertisements; DHCPv6 (stateless) for the DNS
       servers when they say there is more to ask. */
    esp_netif_create_ip6_linklocal(g_sta_netif);
    esp_netif_tcpip_exec([](void *) -> esp_err_t {
      dhcp6_enable_stateless(static_cast<struct netif *>(esp_netif_get_netif_impl(g_sta_netif)));
      return ESP_OK;
    }, nullptr);
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
    auto *event = static_cast<ip_event_got_ip6_t *>(data);
    if (event->esp_netif != g_sta_netif)
      return;
    ESP_LOGI(TAG, "IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    esp_ip6_addr_t a = event->ip6_info.ip;
    const bool usable = esp_netif_ip6_get_addr_type(&a) != ESP_IP6_ADDR_IS_LINK_LOCAL;
    if (usable && g_state != State::Connected) {
      ESP_LOGI(TAG, "Joined by IPv6 (no IPv4 address yet)");
      on_joined();   // an IPv6-only network: no DHCPv4 answer is coming
    } else if (g_addr_cb) {
      g_addr_cb();
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    char address[16];
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
    {
      std::lock_guard<std::mutex> lock(g_ip_mx);
      std::memcpy(g_ip, address, sizeof(g_ip));
    }
    const char *announced = nullptr;
    esp_netif_get_hostname(g_sta_netif, &announced);
    ESP_LOGI(TAG, "Connected, IP %s, hostname \"%s\"", address, announced ? announced : "(unset)");

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
      ESP_LOGI(TAG, "Link: %s, rssi %d, channel %d", ap.phy_11n ? "WiFi 4 (11n)" : "legacy", ap.rssi, ap.primary);
    for (int i = 0; i < 2; i++)   // DHCP's DNS servers, or the fixed ones: the ones tick() keeps in front
      if (esp_netif_get_dns_info(g_sta_netif, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &g_dns4[i]) != ESP_OK ||
          g_dns4[i].ip.type != ESP_IPADDR_TYPE_V4)
        g_dns4[i] = {};
    if (g_state == State::Connected) {   // joined by IPv6 already: the address is new
      if (g_addr_cb)
        g_addr_cb();
      return;
    }
    on_joined();
  }
}


}  // namespace

const char *to_string(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::ApMode: return "ap-mode";
  }
  return "?";
}

bool begin(StateCallback cb) {
  g_cb = std::move(cb);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta_netif = esp_netif_create_default_wifi_sta();
  g_ap_netif = esp_netif_create_default_wifi_ap();

  /* Set once, right after the interface exists and long before DHCP runs, so
     the name is in the very first DHCP request rather than a later renewal. */
  const std::string host = appcfg::device().name;
  esp_err_t hn = esp_netif_set_hostname(g_sta_netif, host.c_str());
  if (hn != ESP_OK)
    ESP_LOGW(TAG, "Could not set hostname: %s", esp_err_to_name(hn));

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &on_wifi_event, nullptr, nullptr));
  g_ipv6 = appcfg::ip().ipv6;
  if (!g_ipv6) {   // no link-local address is made (WIFI_EVENT_STA_CONNECTED); no SLAAC either
    esp_netif_tcpip_exec([](void *) -> esp_err_t {
      netif_set_ip6_autoconfig_enabled(static_cast<struct netif *>(esp_netif_get_netif_impl(g_sta_netif)), 0);
      return ESP_OK;
    }, nullptr);
    ESP_LOGI(TAG, "IPv6 off");
  }

  /* Station only to begin with. The access point is not a permanent feature --
     it is opened when there is no other way in, and closed again once there is.
     Leaving it up would put a second, open network on the air for no reason.
     While it is up the mode is APSTA, so the station side keeps retrying in the
     background and the device rejoins by itself when the router returns. */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  const esp_timer_create_args_t linger = {
      .callback =
          [](void *) {
            if (someone_joined()) {   // still in use: a little later
              esp_timer_start_once(g_ap_linger_timer, kHoldCheckUs);
              return;
            }
            stop_ap();
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_linger",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&linger, &g_ap_linger_timer);

  const esp_timer_create_args_t retry = {
      .callback =
          [](void *) {
            if (g_ap_active && esp_timer_get_time() >= g_by_hand_until && !someone_joined()) {
              ESP_LOGI(TAG, "Retrying \"%s\"", appcfg::wifi().ssid.c_str());
              esp_wifi_connect();
            }
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&retry, &g_ap_retry_timer);

  /* open_hotspot's work, on the timer task like the rest of the hotspot's */
  const esp_timer_create_args_t open = {
      .callback =
          [](void *) {
            ESP_LOGW(TAG, "Hotspot opened by hand; the home network waits %llds", kByHandUs / 1000000);
            g_by_hand_until = esp_timer_get_time() + kByHandUs;
            g_by_hand = true;
            const bool joined = g_state == State::Connected;
            start_ap();  // first: in hotspot mode the station's disconnect is not retried
            if (joined)
              esp_wifi_disconnect();
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_open",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&open, &g_open_timer);

  /* close_hotspot's work: the hotspot opened by hand closed again, the home network joined at once */
  const esp_timer_create_args_t close = {
      .callback =
          [](void *) {
            if (!g_by_hand || g_state != State::ApMode)
              return;
            ESP_LOGW(TAG, "Hotspot closed by hand; joining the home network");
            g_by_hand_until = 0;
            stop_ap();
            try_connect();
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_close",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&close, &g_close_timer);

  const esp_timer_create_args_t sta_retry = {
      .callback = [](void *) { esp_wifi_connect(); },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "sta_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&sta_retry, &g_sta_retry_timer);

  /* Connected, the driver writes the access point's channel into the station's configuration. With 802.11v the
     supplicant clears it at once -- while the driver still counts as connecting, which refuses the change ("sta is
     connecting, cannot set config"). Left there, the channel would be the only one scanned on the next join: an
     access point gone, its neighbours on other channels never found. So cleared here, two seconds later. */
  const esp_timer_create_args_t unpin = {
      .callback =
          [](void *) {
            wifi_config_t sta = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK || (!sta.sta.bssid_set && sta.sta.channel == 0))
              return;
            ESP_LOGI(TAG, "Free to roam: channel %u%s no longer set", sta.sta.channel, sta.sta.bssid_set ? " and BSSID" : "");
            sta.sta.bssid_set = false;
            sta.sta.channel = 0;
            esp_wifi_set_config(WIFI_IF_STA, &sta);
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_unpin",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&unpin, &g_unpin_timer);

  /* Full power unless something has been learned. A brownout is detected by
     name and steps this down; see diagnostics. */
  const int want = appcfg::tx_power_dbm();
  if (want > 0) {
    esp_wifi_set_max_tx_power(static_cast<int8_t>(want * 4));  // API takes quarter dBm
    ESP_LOGI(TAG, "Transmit power limited to %d dBm", want);
  }
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  ESP_ERROR_CHECK(esp_wifi_start());

  try_connect();
  return true;
}

State state() { return g_state; }
std::string ip() {
  std::lock_guard<std::mutex> lock(g_ip_mx);
  return g_ip;
}

std::string ssid() {
  if (g_state == State::ApMode)
    return appcfg::device().name;
  return appcfg::wifi().ssid;
}

int8_t rssi() {
  wifi_ap_record_t ap;
  if (g_state == State::Connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    return ap.rssi;
  return 0;
}

uint8_t channel() {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK)
    return primary;
  return 0;
}

/* What a scanned network's security means for the panel (Net.h Network). A
   network offering WPA and WPA2 is joined with WPA2; OWE is an open network
   with encryption. */
const char *security_of(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_OWE: return "open";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "unsupported";
  }
}

const char *security() {
  wifi_ap_record_t ap;
  if (g_state == State::Connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    return security_of(ap.authmode);
  return "";
}

bool provisioning() { return g_state == State::ApMode; }

bool valid_ip(const appcfg::Ip &i, std::string *error) {
  const auto fail = [&](const char *why) {
    if (error)
      *error = why;
    return false;
  };
  bool ok;
  const uint32_t a = parse_ip(i.address, &ok);
  if (!ok) return fail("the address is not four numbers 0 to 255");
  if (i.prefix < 8 || i.prefix > 30) return fail("the prefix must be 8 to 30");
  const uint32_t mask = 0xFFFFFFFFu << (32 - i.prefix);
  if ((a & ~mask) == 0 || (a & ~mask) == ~mask) return fail("the address is its network's own or broadcast address");
  const uint32_t g = parse_ip(i.gateway, &ok);
  if (!ok) return fail("the gateway is not four numbers 0 to 255");
  if ((g & mask) != (a & mask)) return fail("the gateway is not in the address's network");
  if (g == a) return fail("the gateway is the address itself");
  if (!i.dns1.empty() && (parse_ip(i.dns1, &ok), !ok)) return fail("the first DNS server is not an address");
  if (!i.dns2.empty() && (parse_ip(i.dns2, &ok), !ok)) return fail("the second DNS server is not an address");
  return true;
}

Address address() {
  Address out;
  out.fixed = g_fixed;
  out.fallback = g_fallback;
  esp_netif_ip_info_t info;
  if (g_state != State::Connected || esp_netif_get_ip_info(g_sta_netif, &info) != ESP_OK)
    return out;
  char buf[16];
  const auto str = [&](uint32_t net) {
    const uint32_t h = ntohl(net);
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(h >> 24), static_cast<unsigned>(h >> 16 & 255),
                  static_cast<unsigned>(h >> 8 & 255), static_cast<unsigned>(h & 255));
    return std::string(buf);
  };
  if (info.ip.addr) {   // none on an IPv6-only network
    out.ip = str(info.ip.addr);
    out.gateway = str(info.gw.addr);
    out.prefix = __builtin_popcount(info.netmask.addr);
  }
  // a DNS server: IPv4, or IPv6 (DHCPv6's, or the router's announcement)
  const auto dns = [&](esp_netif_dns_type_t slot) -> std::string {
    esp_netif_dns_info_t d;
    if (esp_netif_get_dns_info(g_sta_netif, slot, &d) != ESP_OK)
      return "";
    if (d.ip.type == ESP_IPADDR_TYPE_V4)
      return d.ip.u_addr.ip4.addr ? str(d.ip.u_addr.ip4.addr) : "";
    ip6_addr_t lw;
    std::memcpy(lw.addr, d.ip.u_addr.ip6.addr, sizeof(lw.addr));
    ip6_addr_clear_zone(&lw);
    if (ip6_addr_isany(&lw))
      return "";
    char b6[48];
    ip6addr_ntoa_r(&lw, b6, sizeof(b6));
    std::string v = b6;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    return v;
  };
  out.dns1 = dns(ESP_NETIF_DNS_MAIN);
  out.dns2 = dns(ESP_NETIF_DNS_BACKUP);
  out.dns3 = dns(ESP_NETIF_DNS_FALLBACK);
  return out;
}

std::vector<Address6> ipv6() {
  std::vector<Address6> out;
  if (g_state != State::Connected || !g_ipv6)
    return out;
  esp_ip6_addr_t a[LWIP_IPV6_NUM_ADDRESSES];
  const int n = esp_netif_get_all_ip6(g_sta_netif, a);
  for (int i = 0; i < n; i++) {
    Address6 x;
    char buf[48];
    ip6_addr_t lw;
    std::memcpy(lw.addr, a[i].addr, sizeof(lw.addr));
    ip6_addr_clear_zone(&lw);
    ip6addr_ntoa_r(&lw, buf, sizeof(buf));
    x.address = buf;
    std::transform(x.address.begin(), x.address.end(), x.address.begin(), [](unsigned char c) { return std::tolower(c); });
    std::memcpy(x.bytes, a[i].addr, 16);
    const esp_ip6_addr_type_t t = esp_netif_ip6_get_addr_type(&a[i]);
    x.kind = t == ESP_IP6_ADDR_IS_LINK_LOCAL ? "link-local" : t == ESP_IP6_ADDR_IS_UNIQUE_LOCAL ? "unique-local" : "global";
    out.push_back(x);
  }
  return out;
}

void on_address(std::function<void()> cb) { g_addr_cb = std::move(cb); }

std::string ipv6_router() {
  if (!g_ipv6 || g_state != State::Connected)
    return "";
  struct Ask {
    ip6_addr_t addr;
    bool found;
  } ask = {};
  esp_netif_tcpip_exec([](void *ctx) -> esp_err_t {   // lwIP's router list, in its own task
    auto *a = static_cast<Ask *>(ctx);
    const struct netif *sta = static_cast<struct netif *>(esp_netif_get_netif_impl(g_sta_netif));
    for (int i = 0; i < LWIP_ND6_NUM_ROUTERS && !a->found; i++) {
      const nd6_neighbor_cache_entry *n = default_router_list[i].neighbor_entry;
      if (n && n->netif == sta && default_router_list[i].invalidation_timer > 0) {
        a->addr = n->next_hop_address;
        a->found = true;
      }
    }
    return ESP_OK;
  }, &ask);
  if (!ask.found)
    return "";
  ip6_addr_clear_zone(&ask.addr);
  char buf[48];
  ip6addr_ntoa_r(&ask.addr, buf, sizeof(buf));
  std::string out = buf;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::vector<TimeServer> time_servers() {
  std::vector<TimeServer> out;
  std::lock_guard<std::mutex> lock(g_sntp_mx);
  if (!g_sntp_started)
    return out;
  for (int i = 0; i < SNTP_MAX_SERVERS; i++) {
    std::string name;
    if (const char *n = esp_sntp_getservername(i); n && *n) {
      name = n;
    } else if (const ip_addr_t *a = esp_sntp_getserver(i); a && !ip_addr_isany(a)) {
      char buf[48];
      ipaddr_ntoa_r(a, buf, sizeof(buf));
      name = buf;
    }
    if (name.empty())
      continue;
    // the configured server first (if any), then the pool; without one, DHCP's in the first place
    const char *source = name == kPool ? "pool" : !g_sntp_server.empty() && name == g_sntp_server ? "configured" : "dhcp";
    out.push_back({name, source});
  }
  return out;
}

void tick() {
  {
    std::lock_guard<std::mutex> lock(g_sntp_mx);
    if (g_sntp_started && g_sntp_server.empty() && !g_fixed && !esp_sntp_getservername(1)) {
      const ip_addr_t *a = esp_sntp_getserver(1);
      if ((!a || ip_addr_isany(a)) && !dhcp_time_server().empty())
        place_pool();   // a renewed lease named DHCP's server again and cleared the fallback
    }
  }
  const auto set = [](const esp_netif_dns_info_t &d) { return d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr; };
  if (!g_ipv6 || g_state != State::Connected || !set(g_dns4[0]))
    return;
  esp_netif_dns_info_t d[2], v6 = {};
  for (int i = 0; i < 2; i++) {
    if (esp_netif_get_dns_info(g_sta_netif, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &d[i]) != ESP_OK)
      return;
    if (d[i].ip.type == ESP_IPADDR_TYPE_V6 && v6.ip.type != ESP_IPADDR_TYPE_V6)
      v6 = d[i];
  }
  if (v6.ip.type != ESP_IPADDR_TYPE_V6)
    return;   // DHCPv6 named none (or they are behind already)
  // the IPv4 ones first; the IPv6 one in the second place if IPv4 named one server, else in the third
  if (d[0].ip.type == ESP_IPADDR_TYPE_V4 && !set(g_dns4[1]))
    return;
  esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &g_dns4[0]);
  esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_BACKUP, set(g_dns4[1]) ? &g_dns4[1] : &v6);
  if (set(g_dns4[1]))
    esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_FALLBACK, &v6);
  ESP_LOGI(TAG, "DNS: the IPv4 servers first, DHCPv6's behind them");
}

void use_dhcp_for_now() {
  if (!g_fixed)
    return;
  ESP_LOGW(TAG, "The fixed address's gateway does not answer: DHCP until the next start");
  g_fallback = true;
  apply_address();         // DHCP on (g_fixed off)
  esp_wifi_disconnect();   // joined again by the retry (the disconnect handler), now asking DHCP
}

bool hotspot_by_hand() { return g_by_hand && g_state == State::ApMode; }

void open_hotspot() {
  if (g_open_timer) {
    esp_timer_stop(g_open_timer);
    esp_timer_start_once(g_open_timer, 1000);
  }
}

void close_hotspot() {
  if (g_close_timer && hotspot_by_hand()) {
    esp_timer_stop(g_close_timer);
    esp_timer_start_once(g_close_timer, 1000);
  }
}

bool scan(std::vector<Network> &out) {
  out.clear();
  /* A scan hops channels, which the driver refuses while the station is in the
     middle of joining. In AP mode it works, at the cost of a short gap for the
     setup clients -- which is the one place a scan is actually needed. */
  if (g_state == State::Connecting) {
    ESP_LOGW(TAG, "Scan refused while connecting");
    return false;
  }
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  esp_err_t err = esp_wifi_scan_start(&cfg, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
    return false;
  }

  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  count = std::min<uint16_t>(count, 32);
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK)
    return false;
  records.resize(count);

  /* The same network shows up once per access point; the strongest one is all
     the user needs to see, and the list comes back sorted by signal already. */
  for (const auto &r : records) {
    const std::string ssid(reinterpret_cast<const char *>(r.ssid));
    if (ssid.empty())
      continue;
    if (std::any_of(out.begin(), out.end(), [&](const Network &n) { return n.ssid == ssid; }))
      continue;
    out.push_back({ssid, r.rssi, r.authmode != WIFI_AUTH_OPEN && r.authmode != WIFI_AUTH_OWE,
                   security_of(r.authmode)});
  }
  return true;
}

Time time_info() {
  Time t;
  t.synced = g_time_synced;
  if (!t.synced)
    return t;

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  t.epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  /* Both clocks are read as close together as they can be, so the difference is
     the offset between them and not a measurement of how long this took. */
  t.boot_epoch_ms = t.epoch_ms - esp_timer_get_time() / 1000;
  return t;
}

void apply_clock() {
  const appcfg::Clock c = appcfg::clock();
  setenv("TZ", c.tz_posix.c_str(), 1);
  tzset();
  ESP_LOGI(TAG, "Timezone %s (%s)", c.tz_name.c_str(), c.tz_posix.c_str());
  /* A new server only matters once SNTP runs; before that it is picked up
     when the station gets its first address. */
  std::lock_guard<std::mutex> lock(g_sntp_mx);
  if (g_sntp_started && c.server == g_sntp_server)
    return;  // only the timezone changed
  if (g_sntp_started) {
    esp_netif_sntp_deinit();
    g_sntp_started = false;
  }
  if (g_sntp_wanted)
    start_sntp_locked();
}

}  // namespace appnet
