#include "netwatch.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <appnet/Net.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

#include <algorithm>
#include <atomic>

namespace netwatch {
namespace {

const char *TAG = "netwatch";

constexpr int64_t kMinute = 60LL * 1000 * 1000;
constexpr int64_t kInterval = kMinute;          // a look at the gateway
constexpr int64_t kRejoinAfter = 3 * kMinute;   // not answering that long: leave and join again
constexpr int64_t kRestartAfter = 5 * kMinute;  // ... and that long: restart (doubled per restart in a row)
constexpr int64_t kRestartMax = 120 * kMinute;
constexpr int64_t kFixedGiveUp = 2 * kMinute; // a fixed address whose gateway never answered: DHCP for now
constexpr int64_t kSettled = 30 * kMinute;      // working that long: the restarts in a row are forgotten

/* Kept across a software restart (not across power): the restarts in a row. */
constexpr uint32_t kMagic = 0x4e574348;  // "NWCH"
RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR uint32_t g_restarts;

std::atomic<int> g_replies{0};
std::atomic<bool> g_done{true};
esp_ping_handle_t g_ping = nullptr;

bool g_armed = false;        // the gateway has answered once
int64_t g_next = 0;          // the next look
int64_t g_failing_since = 0; // 0: answering
int64_t g_working_since = 0; // since when it answers without a pause
bool g_rejoined = false;
int64_t g_joined_at = 0;     // 0: not joined

/* The IPv4 gateway; on a network without IPv4 the IPv6 router (link-local: asked through the station's
   interface, `iface`). */
bool gateway(ip_addr_t &out, uint32_t &iface) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  iface = 0;
  if (!sta)
    return false;
  if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.gw.addr != 0) {
    IP_ADDR4(&out, esp_ip4_addr1(&ip.gw), esp_ip4_addr2(&ip.gw), esp_ip4_addr3(&ip.gw), esp_ip4_addr4(&ip.gw));
    return true;
  }
  const std::string router = appnet::ipv6_router();
  if (router.empty() || !ipaddr_aton(router.c_str(), &out))
    return false;
  iface = esp_netif_get_netif_impl_index(sta);
  return true;
}

/* Three pings, half a second apart; the answers are counted, the end noted. */
bool ask(const ip_addr_t &gw, uint32_t iface) {
  if (g_ping) {
    esp_ping_delete_session(g_ping);
    g_ping = nullptr;
  }
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = gw;
  cfg.interface = iface;
  cfg.count = 3;
  cfg.interval_ms = 500;
  cfg.timeout_ms = 1000;
  cfg.task_stack_size = 3072;
  esp_ping_callbacks_t cb = {};
  cb.on_ping_success = [](esp_ping_handle_t, void *) { g_replies++; };
  cb.on_ping_end = [](esp_ping_handle_t, void *) { g_done = true; };
  g_replies = 0;
  g_done = false;
  if (esp_ping_new_session(&cfg, &cb, &g_ping) != ESP_OK || esp_ping_start(g_ping) != ESP_OK) {
    g_done = true;
    return false;
  }
  return true;
}

int64_t restart_after() {
  const uint32_t n = std::min<uint32_t>(restarts(), 5);
  return std::min(kRestartAfter << n, kRestartMax);
}

}  // namespace

uint32_t restarts() {
  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_restarts = 0;
  }
  return g_restarts;
}

void tick(int64_t now, bool joined) {
  if (!joined) {
    /* The station's own reconnecting and hotspot take care of that. A silence
       that began before goes on counting: joining again is no proof the
       network works -- only the gateway answering is. */
    g_working_since = 0;
    g_joined_at = 0;
    g_next = now + kInterval;
    return;
  }
  if (!g_joined_at)
    g_joined_at = now;
  if (!g_armed && now - g_joined_at > kFixedGiveUp && appnet::address().fixed) {
    ESP_LOGW(TAG, "Fixed address: the gateway has not answered in %lld min", kFixedGiveUp / kMinute);
    g_joined_at = 0;
    appnet::use_dhcp_for_now();
    return;
  }
  if (g_done) {
    // a look has ended (it takes about 3 s): what did it see?
    if (g_ping) {
      const bool answered = g_replies > 0;
      esp_ping_delete_session(g_ping);
      g_ping = nullptr;
      if (answered) {
        if (!g_armed)
          ESP_LOGI(TAG, "Gateway answers; watching the network");
        if (g_failing_since)
          ESP_LOGI(TAG, "Gateway answers again");
        g_armed = true;
        g_failing_since = 0;
        g_rejoined = false;
        if (!g_working_since)
          g_working_since = now;
        if (now - g_working_since > kSettled && restarts() > 0) {
          ESP_LOGI(TAG, "Working again for %lld min: restarts in a row forgotten", kSettled / kMinute);
          g_restarts = 0;
        }
      } else if (g_armed) {
        g_working_since = 0;
        if (!g_failing_since) {
          g_failing_since = now;
          ESP_LOGW(TAG, "Gateway does not answer");
        }
      }
    }
  }
  if (g_failing_since) {
    const int64_t silent = now - g_failing_since;
    if (silent >= restart_after()) {
      ESP_LOGE(TAG, "Network silent for %lld min although joined -- restarting (%u in a row)",
               silent / kMinute, static_cast<unsigned>(restarts() + 1));
      g_restarts = restarts() + 1;
      esp_restart();
    }
    if (silent >= kRejoinAfter && !g_rejoined) {
      g_rejoined = true;
      ESP_LOGW(TAG, "Network silent for %lld min although joined -- leaving and joining again", silent / kMinute);
      esp_wifi_disconnect();  // the station joins again by itself (appnet)
    }
  }
  if (now < g_next || !g_done)
    return;
  g_next = now + kInterval;
  ip_addr_t gw;
  uint32_t iface;
  if (gateway(gw, iface))
    ask(gw, iface);
}

}  // namespace netwatch
