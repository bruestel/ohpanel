/*
   The options of an item -- its command options, else its state options
   (openHAB's commandDescription / stateDescription) -- for lists the panel
   shows, e.g. a station catalogue: a String item whose options are
   "stream URI=station".

   What the panel shows of an option comes from the item's "ohpanel"
   metadata, key "stations": a JSON object, per option label -- the
   station's key, a-z 0-9 -- {"name", "short", "icon", "logo"}, each optional
   (docs/panel-config.md).

   Asked for from the display task, which must not wait for the network: the
   answer comes from a cache, and an item not cached -- or asked for afresh --
   is fetched in a task of its own; the listener hears when it has come, and
   then only when it differs from what was cached. No expiry: openHAB tells
   nobody when options or metadata change (no event for either), so whoever
   shows them asks afresh (the favourites page each time it opens).

   The same answer gives the item's range (stateDescription minimum and
   maximum): a lamp's colour temperature, where the light page has no ends
   of its own; and a group's members, for an entity page that lists them.
*/

#include "appoh/OpenHab.h"
#include "Http.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <strings.h>
#include <mutex>
#include <set>

namespace appoh {
namespace {

const char *const TAG = "appoh";
constexpr size_t kMaxItemBytes = 128 * 1024;   // station logos in the metadata: a few KB each

struct Cached {
  std::vector<Option> options;
  Limits limits;
  std::vector<Member> members;
  int64_t at = 0;
  std::map<std::string, Metadata> metadata;   // the item's own, by namespace
};

/* An item's metadata as openHAB lists it ({"ns": {"value", "config"}}), by namespace. */
std::map<std::string, Metadata> metadata_of(const cJSON *all) {
  std::map<std::string, Metadata> out;
  for (const cJSON *ns = all ? all->child : nullptr; ns; ns = ns->next) {
    Metadata &md = out[ns->string];
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(ns, "value"));
    md.value = v ? v : "";
    const cJSON *config = cJSON_GetObjectItem(ns, "config");
    for (const cJSON *c = config ? config->child : nullptr; c; c = c->next) {
      if (cJSON_IsString(c)) {
        md.config[c->string] = c->valuestring;
      } else if (cJSON_IsNumber(c)) {
        char n[32];
        snprintf(n, sizeof(n), "%.10g", c->valuedouble);
        md.config[c->string] = n;
      } else if (cJSON_IsBool(c)) {
        md.config[c->string] = cJSON_IsTrue(c) ? "true" : "false";
      }
    }
  }
  return out;
}

std::mutex g_mx;
std::map<std::string, Cached> g_cache;
std::set<std::string> g_fetching;    // being fetched (one at a time) or waiting for it
std::deque<std::string> g_waiting;   // ... in this order
bool g_fetcher = false;              // the fetch task runs

void start_next_locked();
std::function<void(const std::string &)> g_listener;
std::string g_selector = "ohpanel";   // the metadata asked for: ours, and the namespaces the pages read

/* The "stations" of the metadata: JSON, as a string (an .items file) or as
   an object (YAML in the main UI); null without. Owned by the caller. */
cJSON *stations_of(const cJSON *config) {
  const cJSON *v = cJSON_GetObjectItem(config, "stations");
  if (cJSON_IsObject(v))
    return cJSON_Duplicate(v, true);
  cJSON *parsed = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : nullptr;
  if (parsed && !cJSON_IsObject(parsed)) {
    cJSON_Delete(parsed);
    parsed = nullptr;
  }
  if (cJSON_IsString(v) && !parsed)
    ESP_LOGW(TAG, "ohpanel metadata: \"stations\" is not a JSON object");
  return parsed;
}

std::string text_of(const cJSON *o, const char *key) {
  const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
  return v ? v : "";
}

void fetch_task(void *arg) {
  std::unique_ptr<std::string> item(static_cast<std::string *>(arg));
  const appcfg::OpenHab cfg = appcfg::openhab();
  std::vector<Option> out;
  Limits range;
  std::vector<Member> members;
  std::map<std::string, Metadata> own;
  bool ok = false;
  if (cfg.configured()) {
    std::string selector;
    {
      std::lock_guard<std::mutex> lock(g_mx);
      selector = g_selector;
    }
    Response r = http_get(cfg, "/rest/items/" + *item + "?metadata=" + selector + "&recursive=true", kMaxItemBytes);  // A-Z a-z 0-9 _; recursive: a group lists
                                                                                       // its members (with their metadata) only so
    if (r.status == 200) {
      cJSON *root = cJSON_ParseWithLength(r.body.data(), r.body.size());
      const cJSON *cmd = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "commandDescription"), "commandOptions");
      const cJSON *description = cJSON_GetObjectItem(root, "stateDescription");
      const cJSON *st = cJSON_GetObjectItem(description, "options");
      if (const cJSON *v = cJSON_GetObjectItem(description, "minimum"); cJSON_IsNumber(v))
        range.has_min = true, range.min = v->valuedouble;
      if (const cJSON *v = cJSON_GetObjectItem(description, "maximum"); cJSON_IsNumber(v))
        range.has_max = true, range.max = v->valuedouble;
      if (const cJSON *v = cJSON_GetObjectItem(description, "step"); cJSON_IsNumber(v) && v->valuedouble > 0)
        range.has_step = true, range.step = v->valuedouble;
      const bool commands = cJSON_IsArray(cmd) && cJSON_GetArraySize(cmd) > 0;
      const cJSON *meta = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "metadata"), "ohpanel"),
                                              "config");
      cJSON *stations = stations_of(meta);
      for (const cJSON *o = commands ? cmd->child : cJSON_IsArray(st) ? st->child : nullptr; o; o = o->next) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(o, commands ? "command" : "value"));
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(o, "label"));
        if (value && *value) {
          Option opt{value, label && *label ? label : value, "", "", "", ""};
          if (const cJSON *s = cJSON_GetObjectItemCaseSensitive(stations, opt.label.c_str())) {
            opt.icon = text_of(s, "icon");
            opt.image = text_of(s, "logo");
            opt.short_name = text_of(s, "short");
            opt.name = text_of(s, "name");
          }
          out.push_back(opt);
        }
      }
      const cJSON *list = cJSON_GetObjectItem(root, "members");
      for (const cJSON *m = cJSON_IsArray(list) ? list->child : nullptr; m; m = m->next) {
        const std::string name = text_of(m, "name"), label = text_of(m, "label");
        if (name.empty())
          continue;
        Member member{name, label.empty() ? name : label, metadata_of(cJSON_GetObjectItem(m, "metadata"))};
        members.push_back(std::move(member));
      }
      own = metadata_of(cJSON_GetObjectItem(root, "metadata"));
      std::stable_sort(members.begin(), members.end(),
                       [](const Member &a, const Member &b) { return strcasecmp(a.label.c_str(), b.label.c_str()) < 0; });
      ok = root != nullptr;
      cJSON_Delete(stations);
      cJSON_Delete(root);
    } else {
      ESP_LOGW(TAG, "Options of %s: %s", item->c_str(),
               r.status ? ("HTTP " + std::to_string(r.status)).c_str() : esp_err_to_name(r.err));
    }
  }
  std::function<void(const std::string &)> listener;
  bool regrouped = false, moving_changed = false;
  {
    std::lock_guard<std::mutex> lock(g_mx);
    g_fetching.erase(*item);
    g_fetcher = false;
    start_next_locked();   // the next one waiting -- one at a time: a TLS handshake each, the CPU's for seconds
    if (ok) {
      const auto it = g_cache.find(*item);
      regrouped = it == g_cache.end() ? !members.empty() : it->second.members != members;
      // the item's own metadata names an item as "moving" (a cover page): that one subscribed too
      moving_changed = moving_items(own) != (it == g_cache.end() ? std::vector<std::string>()
                                                                 : moving_items(it->second.metadata));
      if (it == g_cache.end() || it->second.options != out || !(it->second.limits == range) || regrouped ||
          it->second.metadata != own)
        listener = g_listener;   // only news: a page redrawn for nothing would flicker
      g_cache[*item] = {out, range, members, esp_timer_get_time(), own};
    }
  }
  if (ok) {
    ESP_LOGI(TAG, "Options of %s: %u, members: %u", item->c_str(), static_cast<unsigned>(out.size()),
             static_cast<unsigned>(members.size()));
    if (regrouped || moving_changed)
      members_changed();   // subscribed anew: the members, the items their metadata or the item's own names as "moving"
    if (listener)
      listener(*item);
  }
  vTaskDelete(nullptr);
}

/* The next item waiting fetched in a task of its own -- one at a time: each is a connection to openHAB (over https
   a handshake that keeps a core busy for a second or more), several at once starved the idle tasks into the task
   watchdog. Called with g_mx held. */
void start_next_locked() {
  while (!g_fetcher && !g_waiting.empty()) {
    auto *arg = new std::string(g_waiting.front());
    g_waiting.pop_front();
    if (xTaskCreate(fetch_task, "oh_options", 6144, arg, 3, nullptr) == pdPASS) {
      g_fetcher = true;
    } else {
      g_fetching.erase(*arg);
      delete arg;
    }
  }
}

/* The item's cache entry (null while not cached); fetched in the background when not cached or `refresh` -- in
   turn, after those asked for before. Called with g_mx held. */
const Cached *cached_locked(const std::string &item, bool refresh) {
  const auto it = g_cache.find(item);
  const bool known = it != g_cache.end();
  if ((refresh || !known) && !g_fetching.count(item) && g_waiting.size() < 16) {
    g_fetching.insert(item);
    g_waiting.push_back(item);
    start_next_locked();
  }
  return known ? &it->second : nullptr;
}

}  // namespace

bool options(const std::string &item, std::vector<Option> &out, bool refresh) {
  if (item.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_mx);
  const Cached *c = cached_locked(item, refresh);
  if (c)
    out = c->options;
  return c != nullptr;
}

bool limits(const std::string &item, Limits &out) {
  if (item.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_mx);
  const Cached *c = cached_locked(item, false);
  if (c)
    out = c->limits;
  return c && (c->limits.has_min || c->limits.has_max || c->limits.has_step);
}

bool cached_metadata(const std::string &item, std::map<std::string, Metadata> &out) {
  std::lock_guard<std::mutex> lock(g_mx);
  const auto it = g_cache.find(item);
  if (it == g_cache.end())
    return false;
  out = it->second.metadata;
  return true;
}

bool metadata(const std::string &item, std::map<std::string, Metadata> &out, bool refresh) {
  if (item.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_mx);
  const Cached *c = cached_locked(item, refresh);
  if (c)
    out = c->metadata;
  return c != nullptr;
}

void set_namespaces(const std::vector<std::string> &namespaces) {
  std::string selector = "ohpanel";
  for (const std::string &ns : namespaces) {
    bool plain = !ns.empty();   // a name, nothing for the query or a regular expression
    for (const char c : ns)
      plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    if (plain && ns != "ohpanel")
      selector += "," + ns;
  }
  std::lock_guard<std::mutex> lock(g_mx);
  g_selector = selector;
}

bool members(const std::string &group, std::vector<Member> &out, bool refresh) {
  if (group.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_mx);
  const Cached *c = cached_locked(group, refresh);
  if (c)
    out = c->members;
  return c != nullptr;
}

void set_options_listener(std::function<void(const std::string &item)> listener) {
  std::lock_guard<std::mutex> lock(g_mx);
  g_listener = std::move(listener);
}

}  // namespace appoh
