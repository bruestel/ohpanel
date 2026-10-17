/*
   HTTP server: REST API and the embedded UI.
*/

#pragma once

#include <cJSON.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace appweb {

/* The panel configuration is application-level, so it is handed in rather than
   depended on -- this component stays about HTTP and never needs to know what
   a page or a button is. */
struct PanelApi {
  /* The active configuration as JSON text. */
  std::function<std::string()> load;
  /* Validates, then stores. On failure nothing changes and *error says why. */
  std::function<bool(const std::string &json, std::string *error)> save;
  /* Back to the built-in default. */
  std::function<bool()> reset;
};

struct AssetsInfo {
  uint32_t version = 0;  // of the icon set embedded in the firmware
  size_t size = 0;       // of the whole set
  size_t icons = 0;
};

/* The display's own firmware (.tft) and the icon set. Supplied by the
   application, same reason. */
struct DisplayApi {
  std::function<bool(size_t size, int baud, std::string *error)> tft_begin;
  std::function<bool(const uint8_t *data, size_t len, std::string *error)> tft_write;
  std::function<bool(std::string *error)> tft_end;
  std::function<void()> tft_abort;
  std::function<void(const std::string &command)> command;  // one command to the display
  std::function<void(bool on)> log_commands;                 // every command to the display in the log
  /* The icon set embedded in the firmware (tools/gen_display_assets.py): its
     size and version, and its bytes. */
  std::function<AssetsInfo()> assets_info;
  std::function<const uint8_t *()> assets_data;
};

/* Health and post-mortem, supplied by the application for the same reason. */
struct DiagnosticsApi {
  std::function<std::string()> health_json;
  std::function<size_t()> coredump_size;
  std::function<bool(size_t offset, void *buf, size_t len)> read_coredump;
  std::function<bool()> erase_coredump;
  /* The log since a position (a running byte count); *next: where to go on. */
  std::function<std::string(uint32_t since, uint32_t *next)> read_log;
  /* Adds what only the application knows to GET /info (temperature sensor,
     display), as members of the root object. */
  std::function<void(cJSON *info)> info_extra;
};

/* Called with true when a firmware upload starts and false if it fails, so the
   application can signal it however it likes (later: on the display). */
using OtaHook = std::function<void(bool active)>;

/* Largest panel configuration the API accepts in one request. */
inline constexpr size_t kMaxPanelBytes = 128 * 1024;   // as text; what counts is deflated (appstore::kMaxStoredBytes)

bool begin(PanelApi panel, DiagnosticsApi diagnostics = {}, DisplayApi display = {}, OtaHook ota_hook = {});

/* True once the server has answered anything at all. The firmware update's
   trial period uses it as proof of life: a device serving its setup page is
   reachable, even though it has no station connection to show for it. */
bool has_served();

/* The panel's addresses changed (it joined a network, or opened its own): the
   https certificate is issued for them anew where needed (Tls.h). */
void addresses_changed();

}  // namespace appweb
