/*
   What the Engine's files share (Engine.cpp and the Engine*.cpp of its pages): small helpers for the
   configuration's JSON, the page type, colours. Private to the component.
*/

#pragma once

#include "appdisplay/Engine.h"

#include <map>
#include <string>

namespace appdisplay {
namespace detail {


// Colours of the Arduino firmware, RGB565.
inline constexpr int kWhite = 65535;
inline constexpr int kBlack = 0;
inline constexpr int kBlue = 1055;
inline constexpr int kDarkGrey = 10597;
inline constexpr int kLightGrey = 52857;
inline constexpr int kCoverRowsPerSheet = 4;   // the cover group page's member rows (layout.py COVERS_ROWS_N)

// Pictures of the design.


inline const cJSON *member(const cJSON *obj, const char *key) { return obj ? cJSON_GetObjectItem(obj, key) : nullptr; }

inline std::string str(const cJSON *obj, const char *key, const std::string &fallback = "") {
  const cJSON *v = member(obj, key);
  return cJSON_IsString(v) ? v->valuestring : fallback;
}

/* A configured page's type ("type"), read once instead of compared as text everywhere. */
enum class PageType { None, Buttons, Entities, Cover, Thermostat, Light, Media, MediaFavorites, Weather, Qr, System,
                      Settings, Keypad };
inline PageType type_of(const cJSON *page) {
  static const std::map<std::string, PageType> kTypes = {
      {"buttons", PageType::Buttons}, {"entities", PageType::Entities},   {"cover", PageType::Cover},
      {"thermostat", PageType::Thermostat}, {"light", PageType::Light}, {"media", PageType::Media},
      {"media_favorites", PageType::MediaFavorites}, {"weather", PageType::Weather}, {"qr", PageType::Qr},
      {"system", PageType::System}, {"settings", PageType::Settings}, {"keypad", PageType::Keypad}};
  const cJSON *v = page ? cJSON_GetObjectItem(page, "type") : nullptr;
  const auto it = cJSON_IsString(v) ? kTypes.find(v->valuestring) : kTypes.end();
  return it == kTypes.end() ? PageType::None : it->second;
}

inline bool flag(const cJSON *obj, const char *key, bool fallback = false) {
  const cJSON *v = member(obj, key);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

/* Entry `i` of an array member, null for a gap or past the end. */
inline const cJSON *slot(const cJSON *obj, const char *key, int i) {
  const cJSON *a = member(obj, key);
  if (!cJSON_IsArray(a))
    return nullptr;
  const cJSON *e = cJSON_GetArrayItem(a, i);
  return cJSON_IsObject(e) ? e : nullptr;
}

inline int color_of(const cJSON *obj, const char *key, int fallback) {
  const int c = color565(cJSON_GetStringValue(member(obj, key)));
  return c < 0 ? fallback : c;
}



inline constexpr int64_t kSlideMs = 300;                      // a slider moving sends this often at most

inline double num_of(const cJSON *obj, const char *key, double fallback) {
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

}  // namespace detail
}  // namespace appdisplay
