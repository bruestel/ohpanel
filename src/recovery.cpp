/*
   Recovery hold on the two front buttons.
*/

#include "recovery.h"
#include "board.h"

#include "appcfg/Config.h"
#include "appnet/Net.h"
#include "panel_config.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace recovery {
namespace {

const char *const TAG = "recovery";

/* Long enough that it cannot happen by accident (5 s once did). */
constexpr int64_t kHoldUs = 15 * 1000000LL;

/* Below this the press is ignored entirely, which absorbs contact bounce and
   the moment it takes to get the second finger onto the other button. */
constexpr int64_t kArmUs = 1 * 1000000LL;

int64_t g_pressed_since = 0;
bool g_armed = false;

/* The right button alone, held this long, opens the setup hotspot -- nothing
   is erased. Long, as a press switches a relay; a press held over 2 s does
   not (main.cpp), so giving up halfway switches nothing either. */
constexpr int64_t kHotspotHoldUs = 15 * 1000000LL;
int64_t g_right_since = 0;
bool g_right_done = false;  // opened; waits for the button to be let go

/* Set when the buttons are already down as the firmware starts -- a stuck
   button or a press that began before the reboot. Either way it did not begin
   as a request to erase anything, so it must not become one by being held a
   moment too long. Cleared by letting go. */
bool g_ignore_until_release = false;

void wipe_and_reboot() {
  /* The factory reset, exactly as the web interface's (Web.cpp post_reset):
     every stored setting -- WiFi, password, device (name, timezone, sensor
     ...), openHAB -- and the panel configuration. The panel comes back as if
     freshly flashed, its setup hotspot open. */
  ESP_LOGW(TAG, "Both buttons held %llds: factory reset", kHoldUs / 1000000);
  appcfg::factory_reset();
  panel_config::reset();

  ESP_LOGW(TAG, "Rebooting into setup mode");
  esp_restart();
}

}  // namespace

bool both_pressed() { return board::key_pressed(board::PIN_KEY1) && board::key_pressed(board::PIN_KEY2); }

/* The right button alone: a press that ever had the left one with it belongs
   to the recovery hold and starts over. */
static void hotspot_tick(int64_t now_us) {
  const bool right = board::key_pressed(board::PIN_KEY2);
  if (!right || board::key_pressed(board::PIN_KEY1) || g_ignore_until_release) {
    if (!right)
      g_right_done = false;
    g_right_since = 0;
    return;
  }
  if (g_right_done)
    return;
  if (g_right_since == 0) {
    g_right_since = now_us;
    return;
  }
  if (now_us - g_right_since >= kHotspotHoldUs) {
    g_right_done = true;
    ESP_LOGW(TAG, "Right button held %llds: opening the setup hotspot", kHotspotHoldUs / 1000000);
    appnet::open_hotspot();
  }
}

void begin() {
  if (both_pressed()) {
    g_ignore_until_release = true;
    ESP_LOGW(TAG, "Both buttons are down at startup; ignoring until released");
  }
  g_right_done = board::key_pressed(board::PIN_KEY2);  // down already: counts once let go
}

void tick(int64_t now_us) {
  hotspot_tick(now_us);

  /* A press that was already in progress at startup never counts, however long
     it lasts. Only letting go makes the gesture live again. */
  if (g_ignore_until_release) {
    if (!both_pressed()) {
      g_ignore_until_release = false;
      ESP_LOGI(TAG, "Buttons released; recovery hold armed again");
    }
    return;
  }

  if (!both_pressed()) {
    if (g_armed) {
      ESP_LOGI(TAG, "Recovery hold released, nothing changed");
      g_armed = false;
    }
    g_pressed_since = 0;
    return;
  }

  if (g_pressed_since == 0) {
    g_pressed_since = now_us;
    return;
  }

  const int64_t held = now_us - g_pressed_since;

  if (!g_armed && held >= kArmUs) {
    g_armed = true;
    ESP_LOGW(TAG, "Hold for %llds more for a factory reset", (kHoldUs - kArmUs) / 1000000);
  }

  if (held >= kHoldUs)
    wipe_and_reboot();
}

}  // namespace recovery
