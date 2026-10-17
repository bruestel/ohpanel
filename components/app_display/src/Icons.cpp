/*
   Material Design Icons by name.

   The table comes from the icon set embedded in the firmware
   (tools/gen_display_assets.py has the format).
*/

#include "appdisplay/Icons.h"

#include <cstring>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <miniz.h>

extern "C" const uint32_t assets_version;  // the version of the embedded icon set
#endif

namespace appdisplay {
namespace {

const uint8_t *g_raw = nullptr;
/* Offset of every entry; built once, then binary-searched. */
std::vector<uint32_t> g_index;

}  // namespace

bool icons_use(const uint8_t *raw, size_t len) {
  g_index.clear();
  for (size_t pos = 0; pos < len;) {
    g_index.push_back(static_cast<uint32_t>(pos));
    const void *zero = std::memchr(raw + pos, 0, len - pos);
    if (!zero)
      return false;
    pos = static_cast<const uint8_t *>(zero) - raw + 1 + 3;
  }
  g_raw = raw;
  return true;
}

#ifdef ESP_PLATFORM
namespace {

constexpr char kMagic[8] = {'O', 'H', 'A', 'S', 'S', 'E', 'T', '2'};

/* The icon set, embedded in the firmware (EMBED_FILES). */
extern "C" const uint8_t icon_set_start[] asm("_binary_icon_set_bin_start");
extern "C" const uint8_t icon_set_end[] asm("_binary_icon_set_bin_end");

size_t blob_size() { return static_cast<size_t>(icon_set_end - icon_set_start); }

bool read_header(AssetsHeader &h) {
  if (blob_size() < sizeof(h))
    return false;
  std::memcpy(&h, icon_set_start, sizeof(h));
  return memcmp(h.magic, kMagic, sizeof(kMagic)) == 0 && h.length <= blob_size() - sizeof(h) &&
         h.table_z <= h.length;
}

}  // namespace

bool icons_begin() {
  if (g_raw)
    return true;
  AssetsHeader h;
  if (!read_header(h)) {
    ESP_LOGE("icons", "The embedded icon set is damaged");
    return false;
  }
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc_prefer(h.table_raw, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
  if (!buf) {
    ESP_LOGE("icons", "No memory for the icon table (%u bytes)", static_cast<unsigned>(h.table_raw));
    return false;
  }
  /* Not tinfl_decompress_mem_to_mem: that keeps its decompressor (three
     Huffman tables, about 11 KB) on the stack, which overflowed the main
     task on the first boot of a real panel. On the heap instead. */
  auto *inflator = static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_DEFAULT));
  size_t in_len = h.table_z, got = h.table_raw;
  tinfl_status status = TINFL_STATUS_FAILED;
  if (inflator) {
    tinfl_init(inflator);
    status = tinfl_decompress(inflator, icon_set_start + sizeof(h), &in_len, buf, buf, &got,
                              TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 |
                                  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(inflator);
  }
  if (status != TINFL_STATUS_DONE || got != h.table_raw || !icons_use(buf, h.table_raw)) {
    ESP_LOGE("icons", "Icon table corrupt");
    heap_caps_free(buf);
    return false;
  }
  ESP_LOGI("icons", "%u icons, set %08lx", static_cast<unsigned>(g_index.size()), static_cast<unsigned long>(h.version));
  return true;
}

AssetsInfo assets_info() {
  AssetsInfo i;
  i.version = assets_version;
  i.size = blob_size();
  i.icons = g_index.size();
  return i;
}

const uint8_t *assets_data() { return icon_set_start; }
#endif

std::string icon_glyph(const char *name) {
  if (!g_raw || !name)
    return {};
  size_t lo = 0, hi = g_index.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const char *entry = reinterpret_cast<const char *>(g_raw + g_index[mid]);
    const int c = std::strcmp(entry, name);
    if (c == 0)
      return std::string(entry + std::strlen(entry) + 1, 3);
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return {};
}

size_t icon_count() { return g_index.size(); }

}  // namespace appdisplay
