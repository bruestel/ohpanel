/*
   openHAB's item list for the configuration editor.

   A large installation has well over a thousand items. Kept as one string per
   field that would be thousands of small allocations in internal RAM -- the
   memory the WiFi stack lives on. Instead the list is one block of text, one
   line per item, which as a single large allocation lands in PSRAM and is
   searched linearly; at this size that takes a few milliseconds.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>
#include <cstring>

namespace appoh {
namespace {

const char *const TAG = "appoh";

/* The compact list for 1668 items is 170 KB; this leaves room to grow. */
constexpr size_t kMaxListBytes = 1024 * 1024;
constexpr int64_t kCacheUs = 5 * 60 * 1000000LL;

/* Field separator inside a line: neither item names nor labels contain it. */
constexpr char kSep = '\x1f';

SemaphoreHandle_t g_mux = nullptr;
std::string g_list;  // "name<US>label<US>type\n" per item
int64_t g_fetched_at = 0;
std::string g_fetched_url;

bool fetch(std::string &error) {
  const appcfg::OpenHab cfg = appcfg::openhab();
  if (!cfg.configured()) {
    error = "no openHAB URL configured";
    return false;
  }

  /* Only what the editor shows; metadata= turns off the metadata openHAB
     would otherwise attach to every item. */
  Response r = http_get(cfg, "/rest/items?recursive=false&metadata=&fields=name,label,type", kMaxListBytes);
  if (r.status == 0) {
    error = std::string("openHAB not reachable (") + esp_err_to_name(r.err) + ")";
    return false;
  }
  if (r.status != 200) {
    error = "openHAB answered HTTP " + std::to_string(r.status);
    return false;
  }
  if (r.err == ESP_ERR_INVALID_SIZE) {
    error = "the item list is larger than 1 MB";
    return false;
  }

  cJSON *root = cJSON_ParseWithLength(r.body.data(), r.body.size());
  std::string().swap(r.body);  // the raw text is no longer needed; free it before building the list
  if (!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    error = "openHAB sent no item list";
    return false;
  }

  std::string list;
  list.reserve(static_cast<size_t>(cJSON_GetArraySize(root)) * 64);
  size_t count = 0;
  for (const cJSON *it = root->child; it; it = it->next) {
    const cJSON *name = cJSON_GetObjectItem(it, "name");
    if (!cJSON_IsString(name))
      continue;
    const cJSON *label = cJSON_GetObjectItem(it, "label");
    const cJSON *type = cJSON_GetObjectItem(it, "type");
    list += name->valuestring;
    list += kSep;
    for (const char *c = cJSON_IsString(label) ? label->valuestring : ""; *c; c++)
      list += (*c == '\n' || *c == kSep) ? ' ' : *c;
    list += kSep;
    list += cJSON_IsString(type) ? type->valuestring : "";
    list += '\n';
    count++;
  }
  cJSON_Delete(root);

  g_list.swap(list);
  g_fetched_at = esp_timer_get_time();
  g_fetched_url = cfg.url;
  ESP_LOGI(TAG, "Item list: %u items, %u bytes", static_cast<unsigned>(count), static_cast<unsigned>(g_list.size()));
  return true;
}

/* `needle` is already lower case. */
bool contains_ci(const char *begin, const char *end, const std::string &needle) {
  if (needle.empty())
    return true;
  const size_t n = needle.size();
  for (const char *p = begin; p + n <= end; p++) {
    size_t i = 0;
    while (i < n && std::tolower(static_cast<unsigned char>(p[i])) == needle[i])
      i++;
    if (i == n)
      return true;
  }
  return false;
}

}  // namespace

bool search_items(const std::string &query, const std::string &type, size_t limit, std::vector<ItemInfo> &out,
                  size_t &matches, std::string &error, bool refresh, size_t offset) {
  out.clear();
  matches = 0;
  if (!g_mux)
    g_mux = xSemaphoreCreateMutex();
  xSemaphoreTake(g_mux, portMAX_DELAY);

  const int64_t now = esp_timer_get_time();
  const bool stale = g_list.empty() || refresh || now - g_fetched_at > kCacheUs || g_fetched_url != appcfg::openhab().url;
  if (stale && !fetch(error)) {
    xSemaphoreGive(g_mux);
    return false;
  }

  std::vector<std::string> words;
  std::string word;
  for (char c : query + " ") {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!word.empty())
        words.push_back(word);
      word.clear();
    } else {
      word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }

  // the types asked for, comma-separated; each the beginning of one ("Number" is Number:Temperature too)
  std::vector<std::string> types;
  for (size_t from = 0; from < type.size();) {
    size_t comma = type.find(',', from);
    if (comma == std::string::npos)
      comma = type.size();
    if (comma > from)
      types.push_back(type.substr(from, comma - from));
    from = comma + 1;
  }

  const char *p = g_list.data();
  const char *const end = p + g_list.size();
  while (p < end) {
    const char *eol = static_cast<const char *>(std::memchr(p, '\n', end - p));
    if (!eol)
      eol = end;
    const char *s1 = static_cast<const char *>(std::memchr(p, kSep, eol - p));
    const char *s2 = s1 ? static_cast<const char *>(std::memchr(s1 + 1, kSep, eol - s1 - 1)) : nullptr;
    if (s1 && s2) {
      bool hit = types.empty();
      for (const std::string &t : types)
        hit = hit || (static_cast<size_t>(eol - s2 - 1) >= t.size() && std::strncmp(s2 + 1, t.c_str(), t.size()) == 0);
      for (size_t w = 0; hit && w < words.size(); w++)
        hit = contains_ci(p, s2, words[w]);  // name and label
      if (hit) {
        if (matches >= offset && out.size() < limit)
          out.push_back({std::string(p, s1), std::string(s1 + 1, s2), std::string(s2 + 1, eol)});
        matches++;
      }
    }
    p = eol + 1;
  }

  xSemaphoreGive(g_mux);
  return true;
}

}  // namespace appoh
