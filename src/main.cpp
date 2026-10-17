/*
   ohpanel -- ESP-IDF firmware for the Sonoff NSPanel (EU).

   WiFi with a setup hotspot, web interface, firmware update, persistent
   settings, the panel configuration, the openHAB connection and the display.
*/

#include "board.h"
#include "diagnostics.h"
#include "netwatch.h"
#include "logbuf.h"
#include "panel_config.h"
#include "recovery.h"
#include "temperature.h"

#include "appcfg/Config.h"
#include "appdisplay/Display.h"
#include "appdisplay/Icons.h"
#include "appnet/Net.h"
#include "appoh/OpenHab.h"
#include "appstore/Store.h"
#include "appweb/Web.h"

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>

static const char *const TAG = "ohpanel";

/* Written from the network event task, read by the main loop. */
static std::atomic<appnet::State> g_net{appnet::State::Idle};

static void log_banner() {
  const esp_app_desc_t *app = esp_app_get_description();

  esp_chip_info_t chip;
  esp_chip_info(&chip);

  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);

  ESP_LOGI(TAG, "ohpanel %s (%s %s)", app->version, app->date, app->time);
  ESP_LOGI(TAG, "ESP-IDF %s, %d core(s), rev v%d.%d, %" PRIu32 " MB flash, %u KB PSRAM", IDF_VER, chip.cores,
           chip.revision / 100, chip.revision % 100, flash_size / (1024 * 1024),
           static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
}

/* cJSON allocates one node per value. A 20 KB openHAB snapshot becomes a few
   thousand of them, which is internal RAM the WiFi stack needs more; PSRAM
   takes them when it is there. */
static void *json_malloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

/* Saving or resetting the configuration changes which items the panel needs
   and what the display shows. */
static void apply_panel() {
  appoh::subscribe(panel_config::items(), panel_config::groups(), panel_config::namespaces());
  appdisplay::config_changed(panel_config::load());
}

static bool save_panel(const std::string &json, std::string *error) {
  if (!panel_config::save(json, error))
    return false;
  apply_panel();
  return true;
}

static bool reset_panel() {
  const bool ok = panel_config::reset();
  apply_panel();
  return ok;
}

static void restart() {
  gpio_set_level(board::PIN_DISPLAY_POWER, 1);  // the display off first, so it does not freeze mid-page
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
}

/* The panel's values for the display's info page. */
static appdisplay::SystemInfo system_info() {
  appdisplay::SystemInfo i;
  i.version = esp_app_get_description()->version;
  i.uptime_s = esp_timer_get_time() / 1000000;
  i.ip = appnet::ip();
  const appnet::Address a = appnet::address();
  i.prefix = a.prefix;
  i.gateway = a.gateway;
  i.gateway6 = appnet::ipv6_router();
  for (const std::string &d : {a.dns1, a.dns2, a.dns3})
    if (!d.empty())
      i.dns.push_back(d);
  for (const appnet::Address6 &x : appnet::ipv6())
    i.ipv6.emplace_back(x.kind, x.address);
  if (const auto ts = appnet::time_servers(); !ts.empty())
    i.time_server = ts.front().server;
  i.name = appcfg::device().name + ".local";
  i.rssi = appnet::rssi();
  i.ssid = appnet::state() == appnet::State::Connected ? appnet::ssid() : "";
  // openHAB's address without the scheme and the path: "http://openhab:8080/" -> "openhab:8080"
  std::string oh = appcfg::openhab().url;
  if (const size_t scheme = oh.find("://"); scheme != std::string::npos)
    oh.erase(0, scheme + 3);
  if (const size_t path = oh.find('/'); path != std::string::npos)
    oh.erase(path);
  i.openhab = oh;
  i.openhab_https = appcfg::openhab().url.rfind("https://", 0) == 0;
  i.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  i.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  i.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  i.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  i.temperature = temperature::celsius();
  return i;
}

/* The temperature sensor and the display, for GET /info. */
static void info_extra(cJSON *root) {
  const appcfg::Sensor s = appcfg::sensor();
  cJSON *t = cJSON_AddObjectToObject(root, "temperature");
  const double c = temperature::celsius(), raw = temperature::raw_celsius();
  if (!std::isnan(c)) {
    cJSON_AddNumberToObject(t, "celsius", std::round(c * 10) / 10);
    cJSON_AddNumberToObject(t, "raw", std::round(raw * 10) / 10);
  }
  cJSON_AddNumberToObject(t, "offset_tenths", s.offset_tenths);
  cJSON_AddStringToObject(t, "item", s.item.c_str());

  cJSON *j = cJSON_AddObjectToObject(root, "display");
  const appdisplay::DesignCheck dc = appdisplay::design_check();
  cJSON_AddStringToObject(j, "design_expected", dc.expected.c_str());
  cJSON_AddNumberToObject(j, "link_baud", appdisplay::link_baud());
  cJSON_AddBoolToObject(j, "log_commands", appdisplay::logging_commands());  // until a restart
  if (dc.known) {
    cJSON_AddStringToObject(j, "design", dc.actual.c_str());
    cJSON_AddBoolToObject(j, "design_current", dc.current());
  }
  nextion::DeviceInfo d;
  if (appdisplay::display_info(d)) {
    cJSON_AddStringToObject(j, "model", d.model.c_str());
    cJSON_AddStringToObject(j, "firmware", d.firmware.c_str());
    cJSON_AddStringToObject(j, "mcu", d.mcu.c_str());
    cJSON_AddStringToObject(j, "serial", d.serial.c_str());
    cJSON_AddNumberToObject(j, "flash", d.flash);
  }
}

/* A relay switched and its state kept for the next boot: its pin held at its level otherwise (gpio_hold_en), so a
   restart -- by hand, after an update, the watchdog -- leaves the relay as it
   is; only a power cut lets go (board::init, then the stored state). The
   level goes in first, then the hold is let go and taken again: no glitch. */
static void set_relay(int index, bool on) {
  const gpio_num_t pin = index == 0 ? board::PIN_RELAY1 : board::PIN_RELAY2;
  gpio_set_level(pin, on ? 1 : 0);
  gpio_hold_dis(pin);
  gpio_hold_en(pin);
  if (appcfg::relay(index) != on)
    appcfg::set_relay(index, on);
}

/* A restart in a moment, once (the hotspot ended by a press: see watch_buttons). */
static void restart_soon() {
  static esp_timer_handle_t timer = nullptr;
  if (timer)
    return;
  const esp_timer_create_args_t args = {
      .callback = [](void *) { restart(); },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "restart_soon",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &timer) == ESP_OK)
    esp_timer_start_once(timer, 1500000);
}

/* The front buttons: a press of one button alone acts (relay or item, see
   home.relays); both together belong to the recovery hold, the right one held
   long opens the hotspot (recovery.cpp). A press counts on release, after a
   short debounce, if it was short and the other button stayed up. */
struct ButtonWatch {
  gpio_num_t pin;
  int64_t down_since = 0;
  bool other_seen = false;
};

static void watch_buttons(ButtonWatch (&b)[2], int64_t now) {
  constexpr int64_t kDebounceUs = 40000;
  constexpr int64_t kLongUs = 2000000;  // longer: a hold, given up on -- switches nothing
  for (int i = 0; i < 2; i++) {
    const bool pressed = board::key_pressed(b[i].pin);
    const bool other = board::key_pressed(b[1 - i].pin);
    if (pressed) {
      if (b[i].down_since == 0) {
        b[i].down_since = now;
        b[i].other_seen = false;
      }
      b[i].other_seen = b[i].other_seen || other;
    } else if (b[i].down_since != 0) {
      const int64_t held = now - b[i].down_since;
      if (held > kDebounceUs && held < kLongUs && !b[i].other_seen) {
        appdisplay::hardware_button(i);   // the relay (or item), as always
        // and in the hotspot a press ends it: the one opened by hand closes (the home network again), the
        // one without a network starts the panel afresh -- once the relay's new state is stored (restored
        // after the restart)
        if (appnet::hotspot_by_hand())
          appnet::close_hotspot();
        else if (appnet::provisioning())
          restart_soon();
      }
      b[i].down_since = 0;
    }
  }
}

/* The boot page's time: at least this long after the start, at most this long
   after joining the network (see the main loop). */
constexpr int64_t kBootPageMaxUs = 10 * 1000000LL;

extern "C" void app_main() {
  logbuf::begin();  // first: the web interface's log starts with the banner
  log_banner();
  // a line for every https request ("performing session handshake"): an open web page pushed the rest out of the log
  esp_log_level_set("esp_https_server", ESP_LOG_WARN);

  cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
  cJSON_InitHooks(&hooks);

  board::init();
  diagnostics::begin();
  recovery::begin();

  if (!appcfg::begin())
    ESP_LOGE(TAG, "Config storage unavailable -- settings will not persist");

  appstore::begin();

  /* The relays as they were before the restart (board::init switched them
     off for the few milliseconds until the settings could be read). */
  for (int i = 0; i < 2; i++)
    set_relay(i, appcfg::relay(i));

  temperature::begin();

  /* The timezone from the settings, before the display formats any time. */
  appnet::apply_clock();

  appdisplay::begin(
      nextion::Config{
          .port = UART_NUM_2,
          .tx = board::PIN_DISPLAY_TX,
          .rx = board::PIN_DISPLAY_RX,
          .baud = 921600,   // the design's (its Program.s): no switching at runtime
      },
      appdisplay::Hooks{
          .relay = set_relay,
          .restart = restart,
          .system = system_info,
          .remember = [](const std::string &key, int value) { panel_config::remember_screen(key, value); },
          .power_cycle =
              [] {
                gpio_set_level(board::PIN_DISPLAY_POWER, 1);   // off (the pin switches its supply, low: on)
                vTaskDelay(pdMS_TO_TICKS(800));
                gpio_set_level(board::PIN_DISPLAY_POWER, 0);
                vTaskDelay(pdMS_TO_TICKS(1500));   // its boot, until it listens
              },
      });
  for (int i = 0; i < 2; i++)
    appdisplay::relay_state(i, appcfg::relay(i));

  appoh::begin([](const std::string &item, const appoh::ItemState &) { appdisplay::state_changed(item); });
  apply_panel();

  appnet::on_address([] { appweb::addresses_changed(); });   // a unique local (or a covered global) one: in the certificate
  appnet::begin([](appnet::State s) {
    g_net = s;
    appoh::on_network(s == appnet::State::Connected);
    if (s == appnet::State::Connected || s == appnet::State::ApMode)
      appweb::addresses_changed();   // the https certificate for the address it has now
  });

  appweb::begin(
      appweb::PanelApi{
          .load = panel_config::load,
          .save = save_panel,
          .reset = reset_panel,
      },
      appweb::DiagnosticsApi{
          .health_json = diagnostics::health_json,
          .coredump_size = diagnostics::coredump_size,
          .read_coredump = diagnostics::read_coredump,
          .erase_coredump = diagnostics::erase_coredump,
          .read_log = logbuf::read,
          .info_extra = info_extra,
      },
      appweb::DisplayApi{
          .tft_begin = appdisplay::tft_begin,
          .tft_write = appdisplay::tft_write,
          .tft_end = appdisplay::tft_end,
          .tft_abort = appdisplay::tft_abort,
          .command = appdisplay::send_raw,
          .log_commands = appdisplay::log_commands,
          .assets_info =
              [] {
                const appdisplay::AssetsInfo i = appdisplay::assets_info();
                return appweb::AssetsInfo{.version = i.version, .size = i.size, .icons = i.icons};
              },
          .assets_data = appdisplay::assets_data,
      });

  /* A freshly flashed image boots as PENDING_VERIFY. If it never marks itself
     valid -- because it panics, hangs, or cannot get on the network -- the
     bootloader falls back to the previous slot on the next restart. Confirming
     only after the network is actually up is the point: a firmware that builds
     and boots but cannot be reached is exactly as useless as one that crashes,
     and once the panel is in the wall there is no cable to fix it. */
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &ota_state);
  bool awaiting_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
  const int64_t verify_deadline = esp_timer_get_time() + 60000000;
  if (awaiting_verify)
    ESP_LOGW(TAG, "Running a new image on trial; confirming once the network is up");

  if (netwatch::restarts())
    ESP_LOGW(TAG, "Restarted by the network watch (%u in a row)", static_cast<unsigned>(netwatch::restarts()));

  int64_t next_stats = 0;
  int64_t next_status = 0;
  int64_t next_dns = 0;
  bool display_started = false;
  int64_t joined_at = 0;  // when the network was joined, for the boot page's time
  ButtonWatch buttons[2] = {{board::PIN_KEY1}, {board::PIN_KEY2}};
  const esp_app_desc_t *app = esp_app_get_description();

  for (;;) {
    const int64_t now = esp_timer_get_time();

    diagnostics::feed();
    netwatch::tick(now, g_net == appnet::State::Connected);
    if (now >= next_dns) {   // DHCPv6's DNS servers behind the IPv4 ones (appnet::tick)
      next_dns = now + 5LL * 1000 * 1000;
      appnet::tick();
    }
    recovery::tick(now);
    watch_buttons(buttons, now);
    temperature::tick(now);

    /* What the boot page and the home screen show about the panel; the
       display only redraws what changed. In hotspot mode the boot page stays,
       showing how to reach the setup page. Until home shows, a few times a
       second: it is to come as soon as it can be complete. */
    if (now > next_status) {
      next_status = now + (display_started ? 2000000 : 250000);
      appdisplay::PanelStatus st;
      st.version = app->version;
      st.device = appcfg::device().name;
      st.ip = appnet::ip();
      // the IPv6 address to show: a link-local one only when there is no other (an address all the same, on this net)
      for (const char *kind : {"global", "unique-local", "link-local"})
        for (const appnet::Address6 &x : appnet::ipv6())
          if (st.ipv6.empty() && std::strcmp(x.kind, kind) == 0)
            st.ipv6 = x.address;
      st.wifi = g_net == appnet::State::Connected;
      st.hotspot = appnet::provisioning();
      st.setup = appnet::hotspot_by_hand();
      st.hotspot_ssid = st.hotspot ? appnet::ssid() : "";
      const appoh::Status oh = appoh::status();
      st.openhab = oh.connected;
      appdisplay::status(st);
      /* Home once it can show (nearly) everything: joined, openHAB connected and
         nine in ten of the states there -- at most 10 s after joining, so a
         missing openHAB does not keep the boot page (home then shows what is
         missing). The display keeps the boot page a few seconds anyway, so it
         can be read (Engine::start). */
      if (st.wifi && !joined_at)
        joined_at = now;
      if (!st.wifi)
        joined_at = 0;
      // (the configuration read from NVS only until then: it competes with an upload for the flash)
      const auto states = [&oh] {
        return (oh.connected && oh.received * 10 >= oh.subscribed * 9) || appcfg::openhab().url.empty();
      };
      if (!display_started && joined_at && (now - joined_at >= kBootPageMaxUs || states())) {
        display_started = true;
        appdisplay::start();
      }
    }

    /* Either counts as proof that this image works. Joining a network is the
       usual one; being asked for a page is the one that matters when the
       device came up in setup mode, where there is no station to join and a
       person is standing in front of it typing credentials. */
    if (awaiting_verify && (g_net == appnet::State::Connected || appweb::has_served())) {
      awaiting_verify = false;
      esp_ota_mark_app_valid_cancel_rollback();
      ESP_LOGW(TAG, "New image confirmed, rollback cancelled");
    }
    if (awaiting_verify && now > verify_deadline) {
      ESP_LOGE(TAG, "New image was never reached at all -- rebooting to roll back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    if (now > next_stats) {
      next_stats = now + 60000000;
      ESP_LOGI(TAG, "net: %s%s%s", appnet::to_string(g_net), appnet::ip().empty() ? "" : " ",
               appnet::ip().c_str());
      const appoh::Status oh = appoh::status();
      ESP_LOGI(TAG, "openHAB: %s, %u/%u items, %u reconnects%s%s", oh.connected ? "connected" : "not connected",
               static_cast<unsigned>(oh.received), static_cast<unsigned>(oh.subscribed),
               static_cast<unsigned>(oh.reconnects), oh.last_error.empty() ? "" : ", last error: ",
               oh.last_error.c_str());
      ESP_LOGI(TAG, "health: heap %u (min %u), psram free %u, boot: %s",
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), diagnostics::boot_reason());
      diagnostics::check_stacks();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
