/*
   Decoding of openHAB's item state events.
*/

#include "States.h"

#include <cJSON.h>

namespace appoh {
namespace {

std::string json_str(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}

}  // namespace

bool parse_states(const std::string &data, const std::function<void(const std::string &, const ItemState &)> &handler) {
  cJSON *root = cJSON_ParseWithLength(data.data(), data.size());
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return false;
  }

  for (const cJSON *it = root->child; it; it = it->next) {
    if (!it->string || !cJSON_IsObject(it))
      continue;
    ItemState s;
    s.state = json_str(it, "state");
    /* openHAB leaves displayState out whenever it would equal the state. */
    s.display = cJSON_HasObjectItem(it, "displayState") ? json_str(it, "displayState") : s.state;
    s.type = json_str(it, "type");
    s.unit = json_str(it, "unit");
    const cJSON *num = cJSON_GetObjectItem(it, "numericState");
    if (cJSON_IsNumber(num)) {
      s.numeric = num->valuedouble;
      s.has_numeric = true;
    }
    handler(it->string, s);
  }
  cJSON_Delete(root);
  return true;
}

}  // namespace appoh
