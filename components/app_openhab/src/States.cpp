/*
   Decoding of openHAB's item state events.
*/

#include "States.h"

#include <cJSON.h>

#include <cctype>
#include <cmath>
#include <cstdlib>

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
    } else if (s.type == "Decimal" || s.type == "Quantity" || s.type == "Percent") {
      /* An openHAB that sends no numericState and no unit (older ones): the number read from the state, which
         openHAB writes with a dot and the unit after a space ("8.6 °C"), and a quantity's unit from there too --
         else the panel showed the display text as it was ("8.6 °C" where "9°" belongs: no decimals, no rounding;
         forum #124), took a colour temperature in Kelvin for percent and sent a thermostat's target without its
         unit. */
      // openHAB's form only: a decimal number (no hex, no space before it), after it nothing but a quantity's unit
      const char *text = s.state.c_str();
      const bool starts = std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' || text[0] == '+' ||
                          text[0] == '.';
      char *end = nullptr;
      const double v = starts ? std::strtod(text, &end) : 0;
      std::string unit = end ? end : "";
      unit.erase(0, unit.find_first_not_of(' '));
      unit.erase(unit.find_last_not_of(' ') + 1);
      const bool hex = s.state.find_first_of("xX") != std::string::npos && unit.empty();
      if (starts && end != text && std::isfinite(v) && !hex && (s.type == "Quantity" || unit.empty())) {
        s.numeric = v;
        s.has_numeric = true;
        if (s.type == "Quantity")
          s.unit = unit;   // the number's own, also where openHAB sent another one (that one belongs to no number here)
      }
    }
    handler(it->string, s);
  }
  cJSON_Delete(root);
  return true;
}

}  // namespace appoh
