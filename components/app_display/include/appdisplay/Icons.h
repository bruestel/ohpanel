/*
   Material Design Icons by name, as UTF-8 glyphs of the display's icon font.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace appdisplay {

/* Makes the table usable: on the device it inflates the table of the icon set
   embedded in the firmware (into PSRAM when there is some). False if that
   fails; lookups then find nothing. */
bool icons_begin();

/* The icon set as embedded (tools/gen_display_assets.py). */
struct AssetsHeader {
  char magic[8];
  uint32_t version;  // CRC-32 of everything after the header
  uint32_t length;   // of everything after the header
  uint32_t count;
  uint32_t table_raw;
  uint32_t table_z;
  uint32_t size;  // of an icon picture, in pixels
};
static_assert(sizeof(AssetsHeader) == 32);

struct AssetsInfo {
  uint32_t version = 0;
  size_t size = 0;  // of the whole set
  size_t icons = 0;
};
AssetsInfo assets_info();

/* The embedded set (header and the rest, assets_info().size bytes), for the web interface. */
const uint8_t *assets_data();

/* Uses an already inflated table ("name\0" + 3 bytes per icon, sorted) --
   for the host tests. The memory must outlive the lookups. */
bool icons_use(const uint8_t *raw, size_t len);

/* The glyph for an icon name, or "" if there is no such icon. */
std::string icon_glyph(const char *name);

size_t icon_count();

}  // namespace appdisplay
