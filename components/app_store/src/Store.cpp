/*
   Writable storage for the panel configuration: deflated JSON in the NVS partition "panel".
*/

#include "appstore/Store.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <miniz.h>   // the ESP32's ROM has deflate and inflate: no code of our own for them
#include <nvs.h>
#include <nvs_flash.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace appstore {
namespace {

const char *const TAG = "appstore";
const char *const kPartition = "panel";
const char *const kNamespace = "panel";

/* A stored document: this magic, the raw length (4 bytes, little endian), then the zlib stream
   (with its Adler-32: a damaged value is noticed, not parsed). */
const char kMagic[4] = {'O', 'H', 'Z', '1'};
constexpr size_t kHeader = 8;

bool g_ready = false;

/* The (de)compressor's state -- tens of KB -- from PSRAM where there is some. */
void *big_alloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(size);
}

mz_bool append(const void *buf, int len, void *user) {
  auto *out = static_cast<std::string *>(user);
  out->append(static_cast<const char *>(buf), len);
  return out->size() <= kHeader + kMaxRawBytes;   // no gain: stop
}

/* The document deflated behind the header; empty if the compressor could not run. */
std::string deflate(const std::string &raw) {
  std::string out(kMagic, sizeof(kMagic));
  const uint32_t n = raw.size();
  out.append(reinterpret_cast<const char *>(&n), 4);   // the ESP32 is little endian
  auto *d = static_cast<tdefl_compressor *>(big_alloc(sizeof(tdefl_compressor)));
  if (!d)
    return {};
  constexpr int kFlags = static_cast<int>(TDEFL_WRITE_ZLIB_HEADER) | static_cast<int>(TDEFL_DEFAULT_MAX_PROBES);
  const bool ok = tdefl_init(d, append, &out, kFlags) == TDEFL_STATUS_OKAY &&
                  tdefl_compress_buffer(d, raw.data(), raw.size(), TDEFL_FINISH) == TDEFL_STATUS_DONE;
  free(d);
  return ok ? out : std::string();
}

/* The document back; empty if the value is not one of ours or is damaged. */
std::string inflate(const std::string &stored) {
  if (stored.size() <= kHeader || std::memcmp(stored.data(), kMagic, sizeof(kMagic)) != 0)
    return {};
  uint32_t n = 0;
  std::memcpy(&n, stored.data() + 4, 4);
  if (n == 0 || n > kMaxRawBytes)
    return {};
  auto *r = static_cast<tinfl_decompressor *>(big_alloc(sizeof(tinfl_decompressor)));
  if (!r)
    return {};
  tinfl_init(r);
  std::string out(n, '\0');
  size_t in_size = stored.size() - kHeader, out_size = n;
  auto *dst = reinterpret_cast<mz_uint8 *>(out.data());
  const tinfl_status st = tinfl_decompress(r, reinterpret_cast<const mz_uint8 *>(stored.data() + kHeader), &in_size, dst, dst,
                                           &out_size, static_cast<int>(TINFL_FLAG_PARSE_ZLIB_HEADER) |
                                                          static_cast<int>(TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  free(r);
  if (st != TINFL_STATUS_DONE || out_size != n)
    return {};
  return out;
}

std::string kb(size_t bytes) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  return buf;
}

}  // namespace

bool valid_slug(const std::string &slug) {
  if (slug.empty() || slug.size() > 15)
    return false;
  for (char c : slug)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return slug.front() != '-' && slug.back() != '-';
}

bool begin() {
  esp_err_t err = nvs_flash_init_partition(kPartition);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    /* Never used (or written by another NVS version): an empty partition is the normal state of
       a freshly flashed board. */
    ESP_LOGW(TAG, "Formatting the panel partition (%s)", esp_err_to_name(err));
    nvs_flash_erase_partition(kPartition);
    err = nvs_flash_init_partition(kPartition);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "No panel partition (%s): the partition table is an older one -- flash over serial "
                  "(docs/flashing.md); the panel runs with its default configuration", esp_err_to_name(err));
    return false;
  }
  g_ready = true;
  const Usage u = usage();
  ESP_LOGI(TAG, "Panel storage: %u of %u bytes used, the configuration %u bytes deflated",
           static_cast<unsigned>(u.used), static_cast<unsigned>(u.total), static_cast<unsigned>(u.stored));
  return true;
}

bool available() { return g_ready; }

bool exists(const std::string &slug) {
  if (!g_ready || !valid_slug(slug))
    return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &h) != ESP_OK)
    return false;
  size_t len = 0;
  const bool found = nvs_get_blob(h, slug.c_str(), nullptr, &len) == ESP_OK && len > 0;
  nvs_close(h);
  return found;
}

std::string load(const std::string &slug) {
  if (!g_ready || !valid_slug(slug))
    return {};
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &h) != ESP_OK)
    return {};
  size_t len = 0;
  std::string stored;
  if (nvs_get_blob(h, slug.c_str(), nullptr, &len) == ESP_OK && len > 0 && len <= kHeader + kMaxRawBytes) {
    stored.resize(len);
    if (nvs_get_blob(h, slug.c_str(), stored.data(), &len) != ESP_OK)
      stored.clear();
  }
  nvs_close(h);
  if (stored.empty())
    return {};
  std::string json = inflate(stored);
  if (json.empty())
    ESP_LOGE(TAG, "Stored \"%s\" cannot be read (damaged?); ignoring it", slug.c_str());
  return json;
}

bool save(const std::string &slug, const std::string &json, std::string *error) {
  const auto fail = [&](const std::string &why) {
    if (error)
      *error = why;
    ESP_LOGE(TAG, "Save refused: %s", why.c_str());
    return false;
  };
  if (!g_ready)
    return fail("no panel partition: the partition table is an older one -- flash over serial (docs/flashing.md)");
  if (!valid_slug(slug))
    return fail("name must be 1-15 lowercase letters, digits or dashes");
  if (json.empty())
    return fail("empty document");
  if (json.size() > kMaxRawBytes)
    return fail("the configuration is " + kb(json.size()) + ", the panel takes " + kb(kMaxRawBytes) + " at most");
  const std::string stored = deflate(json);
  if (stored.empty())
    return fail("could not compress the configuration (out of memory?)");
  if (stored.size() > kMaxStoredBytes)
    return fail("the configuration is " + kb(stored.size()) + " compressed (" + kb(json.size()) +
                " as text), the panel keeps " + kb(kMaxStoredBytes) + " compressed at most");
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &h) != ESP_OK)
    return fail("could not open the panel partition");
  const esp_err_t err = nvs_set_blob(h, slug.c_str(), stored.data(), stored.size());
  const esp_err_t committed = err == ESP_OK ? nvs_commit(h) : err;
  nvs_close(h);
  if (committed != ESP_OK)
    return fail(std::string("could not store the configuration: ") + esp_err_to_name(committed));
  ESP_LOGI(TAG, "Stored \"%s\": %u bytes, %u deflated", slug.c_str(), static_cast<unsigned>(json.size()),
           static_cast<unsigned>(stored.size()));
  return true;
}

bool erase(const std::string &slug) {
  if (!g_ready || !valid_slug(slug))
    return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &h) != ESP_OK)
    return false;
  const bool gone = nvs_erase_key(h, slug.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (gone)
    ESP_LOGI(TAG, "Erased \"%s\"", slug.c_str());
  return gone;
}

Usage usage() {
  Usage u;
  if (!g_ready)
    return u;
  nvs_stats_t st = {};
  if (nvs_get_stats(kPartition, &st) == ESP_OK) {
    u.total = st.total_entries * 32;   // an NVS entry is 32 bytes
    u.used = st.used_entries * 32;
  }
  nvs_handle_t h;
  if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &h) == ESP_OK) {
    size_t len = 0;
    if (nvs_get_blob(h, "panel", nullptr, &len) == ESP_OK)   // the configuration (src/panel_config.cpp kSlug)
      u.stored = len;
    nvs_close(h);
  }
  return u;
}

}  // namespace appstore
