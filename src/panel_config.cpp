/*
   The panel configuration.
*/

#include "panel_config.h"

#include "appstore/Store.h"
#include "panel/Schema.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

namespace panel_config {
namespace {

const char *const TAG = "panel";

/* The file name on the storage partition. */
const char *const kSlug = "panel";

const char *const kDefault = "{\n  \"version\": 1\n}\n";

void collect_items(const cJSON *node, std::vector<std::string> &out) {
  for (const cJSON *it = node ? node->child : nullptr; it; it = it->next) {
    if (it->string && std::string(it->string) == "item" && cJSON_IsString(it) && it->valuestring[0])
      out.emplace_back(it->valuestring);
    if (cJSON_IsObject(it) || cJSON_IsArray(it))
      collect_items(it, out);
  }
}

}  // namespace

std::string load() {
  std::string json = appstore::load(kSlug);
  return json.empty() ? std::string(kDefault) : json;
}

bool save(const std::string &json, std::string *error) {
  std::vector<panel::Issue> issues;
  if (!panel::validate(json, issues)) {
    /* One line per issue; the web interface shows them as a list. */
    if (error) {
      error->clear();
      for (const auto &issue : issues)
        *error += (error->empty() ? "" : "\n") + (issue.path.empty() ? "" : issue.path + ": ") + issue.message;
    }
    return false;
  }
  return appstore::save(kSlug, json, error);
}

bool reset() {
  if (!appstore::exists(kSlug))
    return true;
  const bool ok = appstore::erase(kSlug);
  if (ok)
    ESP_LOGI(TAG, "Configuration reset to the default");
  return ok;
}

bool remember_screen(const std::string &key, int value) {
  const std::string json = load();
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  if (!root)
    return false;
  cJSON *display = cJSON_GetObjectItem(root, "display");
  if (!display)
    display = cJSON_AddObjectToObject(root, "display");
  cJSON *screen = cJSON_GetObjectItem(display, "screen");
  if (!screen)
    screen = cJSON_AddObjectToObject(display, "screen");
  cJSON *v = cJSON_GetObjectItem(screen, key.c_str());
  if (cJSON_IsObject(v)) {
    cJSON *n = cJSON_GetObjectItem(v, "value");
    if (cJSON_IsNumber(n))
      cJSON_SetNumberValue(n, value);
    else
      cJSON_AddNumberToObject(v, "value", value);
  } else {
    cJSON_DeleteItemFromObject(screen, key.c_str());
    cJSON_AddNumberToObject(screen, key.c_str(), value);
  }
  char *text = cJSON_Print(root);
  cJSON_Delete(root);
  std::string error;
  const bool ok = text && save(text, &error);
  cJSON_free(text);
  if (ok)
    ESP_LOGI(TAG, "display.screen.%s = %d kept", key.c_str(), value);
  else
    ESP_LOGW(TAG, "display.screen.%s not kept: %s", key.c_str(), error.c_str());
  return ok;
}

std::vector<std::string> items() {
  const std::string json = load();
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  std::vector<std::string> out;
  collect_items(root, out);
  cJSON_Delete(root);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

namespace {
/* The string members `key` of the pages, sorted and unique. */
std::vector<std::string> of_pages(const char *key) {
  const std::string json = load();
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  std::vector<std::string> out;
  const cJSON *pages = cJSON_GetObjectItem(root, "pages");
  for (const cJSON *p = cJSON_IsArray(pages) ? pages->child : nullptr; p; p = p->next) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(p, key));
    if (v && *v)
      out.emplace_back(v);
  }
  cJSON_Delete(root);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
}  // namespace

std::vector<std::string> groups() { return of_pages("group"); }
/* The pages' "metadata" -- and "cover" for a cover page without one (its style, presets: Engine::cover_look). */
std::vector<std::string> namespaces() {
  std::vector<std::string> out = of_pages("metadata");
  const std::string json = load();
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  const cJSON *pages = cJSON_GetObjectItem(root, "pages");
  for (const cJSON *p = cJSON_IsArray(pages) ? pages->child : nullptr; p; p = p->next) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(p, "type"));
    if (type && std::strcmp(type, "cover") == 0 && !cJSON_GetObjectItem(p, "metadata") &&
        std::find(out.begin(), out.end(), "cover") == out.end())
      out.emplace_back("cover");
  }
  cJSON_Delete(root);
  return out;
}

}  // namespace panel_config
