/*
   Validation of the panel configuration.

   Hand-written rather than driven by a JSON Schema document: the rules that
   matter most -- a button opening a page that exists, a page id used once --
   cross-reference parts of the document, which a schema validator on a
   microcontroller would not check anyway.
*/

#include "panel/Schema.h"

#include <cJSON.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace panel {
namespace {

/* "HH:MM", 00:00 to 23:59: a time of day (the night's from and to, a date rule's). */
bool clock_time(const char *t) {
  int h, m;
  char rest;
  return t && std::strlen(t) == 5 && std::sscanf(t, "%d:%d%c", &h, &m, &rest) == 2 && h >= 0 && h <= 23 && m >= 0 &&
         m <= 59;
}

/* What the display offers per page type; more would have nowhere to go. */
constexpr int kMaxButtons = 8;
constexpr int kMaxStatusIcons = 10;
constexpr int kMaxValueRows = 2;
constexpr int kMaxShortcuts = 7;
constexpr int kMaxInfoLines = 2;
constexpr int kMaxRelays = 2;
constexpr int kMaxExtras = 6;   // the weather page's cards
constexpr int kMaxDays = 7;
constexpr int kMaxRows = 64;
constexpr int kMaxPages = 64;

const char *const kColors[] = {"white", "black", "red",  "green",      "blue",
                               "yellow", "brown", "grey", "light-grey", "dark-grey"};
// the theme's colours, wherever a colour goes -- not in the theme itself
const char *const kThemeColors[] = {"theme-accent", "theme-text", "theme-dim"};
const char *const kPageTypes[] = {"buttons", "entities", "cover", "light", "media", "weather", "qr", "system",
                                  "media_favorites", "thermostat", "keypad"};
/* The settings page is built in: always there, never configured; a page
   refers to it by this id like to any other. */
const char *const kSettingsId = "settings";
/* So is the info page; a page "info" (of type "system") only gives it a title. */
const char *const kInfoId = "info";

/* The page types that may be in the swipe row (besides home): those without gestures of their own across the page --
   a list's or the weather's sheets come before the next page (Engine::swipe); the thermostat's ring takes its own
   (a swipe there is none), the keypad's keys and buttons ignore a finger that moved (the display design). */
bool swipe_type(const std::string &t) {
  return t == "buttons" || t == "entities" || t == "weather" || t == "qr" || t == "system" || t == "thermostat" ||
         t == "keypad";
}

class Validator {
 public:
  explicit Validator(std::vector<Issue> &issues) : issues_(issues) {}

  void document(const cJSON *root) {
    if (!cJSON_IsObject(root)) {
      fail("", "the configuration must be a JSON object");
      return;
    }
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint != kSchemaVersion || version->valuedouble != kSchemaVersion) {
      fail("version", "must be " + std::to_string(kSchemaVersion));
      return;  // the rest would be read against the wrong rules
    }
    allowed(root, "", {"version", "display", "home", "swipe", "pages"});
    if (const cJSON *d = cJSON_GetObjectItem(root, "display"))
      display(d, "display");

    /* Page ids first: everything else refers to them. */
    const cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (pages && !cJSON_IsArray(pages))
      fail("pages", "must be an array");
    int index = 0;
    for (const cJSON *p = cJSON_IsArray(pages) ? pages->child : nullptr; p; p = p->next, index++) {
      const cJSON *id = cJSON_GetObjectItem(p, "id");
      if (cJSON_IsString(id) && std::strcmp(id->valuestring, kSettingsId) == 0)
        fail(at("pages", index) + ".id", "\"settings\" is the panel's settings page, which is built in");
      else if (cJSON_IsString(id) && std::strcmp(id->valuestring, kInfoId) == 0 &&
               !(cJSON_IsString(cJSON_GetObjectItem(p, "type")) &&
                 std::strcmp(cJSON_GetObjectItem(p, "type")->valuestring, "system") == 0))
        fail(at("pages", index) + ".id", "\"info\" is the panel's info page, which is built in (of type \"system\")");
      else if (cJSON_IsString(id) && !ids_.insert(id->valuestring).second)
        fail(at("pages", index) + ".id", "\"" + std::string(id->valuestring) + "\" is used by another page");
      else if (cJSON_IsString(id) && cJSON_IsString(cJSON_GetObjectItem(p, "type")))
        types_[id->valuestring] = cJSON_GetObjectItem(p, "type")->valuestring;
    }
    if (index > kMaxPages)
      fail("pages", "at most " + std::to_string(kMaxPages) + " pages");

    index = 0;
    for (const cJSON *p = cJSON_IsArray(pages) ? pages->child : nullptr; p; p = p->next, index++)
      page(p, at("pages", index));

    if (const cJSON *home_obj = cJSON_GetObjectItem(root, "home"))
      home(home_obj, "home");

    if (const cJSON *swipe = cJSON_GetObjectItem(root, "swipe")) {
      if (!cJSON_IsArray(swipe)) {
        fail("swipe", "must be an array of page ids");
      } else {
        // its first page is the start page (home unless the row begins with another one; home only if named)
        index = 0;
        std::set<std::string> seen;
        for (const cJSON *s = swipe->child; s; s = s->next, index++) {
          if (cJSON_IsString(s) && !seen.insert(s->valuestring).second)
            fail(at("swipe", index), "is in the swipe row twice");
          if (cJSON_IsString(s) && std::strcmp(s->valuestring, "home") == 0)
            continue;
          page_ref(s, at("swipe", index));
          // only these types (Engine::swipe_row skips others too); the rest open from a tile or a shortcut
          if (!cJSON_IsString(s))
            continue;
          const std::string id = s->valuestring;
          const std::string type = id == kInfoId ? "system" : id == kSettingsId ? "settings"
                                   : types_.count(id) ? types_[id] : "";
          if (!type.empty() && !swipe_type(type))
            fail(at("swipe", index), "only button, entities, weather, QR and info pages can be in the swipe row; a page "
                                     "of type \"" + type + "\" opens from a tile or a shortcut");
        }
      }
    }
  }

 private:
  std::vector<Issue> &issues_;
  std::set<std::string> ids_;
  std::map<std::string, std::string> types_;   // page id -> type

  void fail(const std::string &path, const std::string &message) {
    if (issues_.size() < kMaxIssues)
      issues_.push_back({path, message});
  }

  static std::string at(const std::string &path, int index) { return path + "[" + std::to_string(index) + "]"; }
  static std::string member(const std::string &path, const char *name) {
    return path.empty() ? name : path + "." + name;
  }

  /* Unknown members are refused rather than ignored: a typo such as "itme"
     would otherwise silently do nothing, which is the hardest kind of mistake
     to find on a wall panel. */
  void allowed(const cJSON *obj, const std::string &path, std::initializer_list<const char *> names) {
    for (const cJSON *it = obj->child; it; it = it->next) {
      bool known = false;
      for (const char *n : names)
        known = known || (it->string && std::strcmp(it->string, n) == 0);
      if (!known)
        fail(member(path, it->string ? it->string : "?"), "unknown member");
    }
  }

  /* An object, or a report that it is not. */
  bool object(const cJSON *v, const std::string &path) {
    if (cJSON_IsObject(v))
      return true;
    fail(path, "must be an object");
    return false;
  }

  void string(const cJSON *obj, const char *name, const std::string &path, bool required, size_t max_len = 200) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (!v) {
      if (required)
        fail(member(path, name), "is required");
      return;
    }
    if (!cJSON_IsString(v))
      fail(member(path, name), "must be a string");
    else if (std::strlen(v->valuestring) > max_len)
      fail(member(path, name), "longer than " + std::to_string(max_len) + " characters");
  }

  void boolean(const cJSON *obj, const char *name, const std::string &path) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (v && !cJSON_IsBool(v))
      fail(member(path, name), "must be true or false");
  }

  void one_of(const cJSON *obj, const char *name, const std::string &path, std::initializer_list<const char *> values,
              bool required = false) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (!v) {
      if (required)
        fail(member(path, name), "is required");
      return;
    }
    if (cJSON_IsString(v))
      for (const char *x : values)
        if (std::strcmp(v->valuestring, x) == 0)
          return;
    std::string list;
    for (const char *x : values)
      list += (list.empty() ? "" : ", ") + std::string(x);
    fail(member(path, name), "must be one of: " + list);
  }

  static bool valid_item_name(const char *s) {
    if (!*s)
      return false;
    for (; *s; s++)
      if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
        return false;
    return true;
  }

  static bool valid_slug(const char *s) {
    const size_t n = std::strlen(s);
    if (n == 0 || n > 24 || s[0] == '-' || s[n - 1] == '-')
      return false;
    for (; *s; s++)
      if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
        return false;
    return true;
  }

  void item_name(const cJSON *v, const std::string &path) {
    if (!cJSON_IsString(v) || !valid_item_name(v->valuestring))
      fail(path, "must be an openHAB item name (A-Z a-z 0-9 _)");
  }

  /* `name` holding {"item": "..."}; required unless said otherwise. A weather
     icon reference may also name its icon set ("codes"). */
  void item_ref(const cJSON *obj, const char *name, const std::string &path, bool required = false,
                bool weather = false) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (!v || cJSON_IsNull(v)) {
      if (required)
        fail(member(path, name), "is required");
      return;
    }
    const std::string p = member(path, name);
    if (!object(v, p))
      return;
    if (weather)
      allowed(v, p, {"item", "codes"});
    else
      allowed(v, p, {"item", "format", "factor", "decimals"});
    decimals(v, p);
    item_name(cJSON_GetObjectItem(v, "item"), member(p, "item"));
    format(v, p);
    if (const cJSON *codes = cJSON_GetObjectItem(v, "codes"))
      if (!cJSON_IsString(codes) || !valid_slug(codes->valuestring))
        fail(member(p, "codes"), "must be an icon set id such as \"openweathermap\"");
  }

  /* display.screen: brightness, idle brightness and time, the night. */
  void screen_settings(const cJSON *s, const std::string &path) {
    if (!object(s, path))
      return;
    allowed(s, path, {"brightness", "idle_brightness", "idle_after", "night"});
    screen_number(s, "brightness", path, 1, 100, "1 to 100 (%)");
    screen_number(s, "idle_brightness", path, 0, 100, "0 (off) to 100 (%)");
    screen_number(s, "idle_after", path, 0, 3600, "0 (never) to 3600 (seconds)");
    const cJSON *n = cJSON_GetObjectItem(s, "night");
    if (!n)
      return;
    const std::string p = member(path, "night");
    if (!object(n, p))
      return;
    allowed(n, p, {"from", "to", "item"});
    if (cJSON_GetObjectItem(n, "item")) {
      item_name(cJSON_GetObjectItem(n, "item"), member(p, "item"));
      if (cJSON_GetObjectItem(n, "from") || cJSON_GetObjectItem(n, "to"))
        fail(p, "either \"from\" and \"to\" or an \"item\", not both");
      return;
    }
    for (const char *k : {"from", "to"}) {
      const cJSON *t = cJSON_GetObjectItem(n, k);
      if (!cJSON_IsString(t) || !clock_time(t->valuestring))
        fail(member(p, k), "must be a time such as \"23:00\"");
    }
  }

  /* A number of display.screen: the number, or {"value", "item"} -- the item
     (a Dimmer or Number) is kept in step with it. */
  void screen_number(const cJSON *s, const char *name, const std::string &path, int low, int high, const char *range) {
    const cJSON *v = cJSON_GetObjectItem(s, name);
    if (!v)
      return;
    const std::string p = member(path, name);
    if (cJSON_IsObject(v)) {
      allowed(v, p, {"value", "item"});
      item_name(cJSON_GetObjectItem(v, "item"), member(p, "item"));
      v = cJSON_GetObjectItem(v, "value");
      if (!v)
        return;
      if (!cJSON_IsNumber(v) || v->valuedouble != v->valueint || v->valueint < low || v->valueint > high)
        fail(member(p, "value"), std::string("must be ") + range);
      return;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != v->valueint || v->valueint < low || v->valueint > high)
      fail(p, std::string("must be ") + range + ", or {\"value\", \"item\"}");
  }

  /* "format": "date", "time", "datetime" or a pattern (format_pattern in
     app_display: %.1f, %d, %,.0f, %s, %unit%, %%); "factor": a number other
     than 0 that multiplies the state first. */
  void format(const cJSON *obj, const std::string &path) {
    if (const cJSON *f = cJSON_GetObjectItem(obj, "factor"))
      if (!cJSON_IsNumber(f) || f->valuedouble == 0)
        fail(member(path, "factor"), "must be a number other than 0, e.g. 0.001 (W -> kW)");
    const cJSON *v = cJSON_GetObjectItem(obj, "format");
    if (!v)
      return;
    const std::string p = member(path, "format");
    const char *s = cJSON_IsString(v) ? v->valuestring : nullptr;
    if (!s || !*s || std::strlen(s) > 48) {
      fail(p, "must be \"date\", \"time\", \"datetime\", a pattern or a text of up to 48 characters");
      return;
    }
    if (!std::strchr(s, '%'))
      return;  // "date", "time", "datetime" or a fixed text
    for (const char *c = s; (c = std::strchr(c, '%')); ) {
      if (c[1] == '%') { c += 2; continue; }
      if (std::strncmp(c, "%unit%", 6) == 0) { c += 6; continue; }
      const char *k = c + 1;
      if (*k == ',') k++;
      if (*k == '.') {
        k++;
        if (!std::isdigit(static_cast<unsigned char>(*k)) || *k > '6') {
          fail(p, "has a placeholder with more than 6 decimals or none after the \".\" -- e.g. %.1f");
          return;
        }
        k++;
      }
      if (*k != 'f' && *k != 'd' && *k != 's') {
        fail(p, std::string("has an unknown placeholder at \"") + std::string(c).substr(0, 6) +
                    "\" -- %.1f, %d, %,.0f, %s, %unit% or %% (a percent sign)");
        return;
      }
      c = k + 1;
    }
  }

  /* "decimals": how many a number shows, 0 to 3. */
  void decimals(const cJSON *obj, const std::string &path) {
    const cJSON *d = cJSON_GetObjectItem(obj, "decimals");
    if (d && (!cJSON_IsNumber(d) || d->valueint < 0 || d->valueint > 3 || d->valuedouble != d->valueint))
      fail(member(path, "decimals"), "must be 0, 1, 2 or 3");
  }

  /* A field of an info line: an item, a fixed text or (the left field) an
     icon; with a text colour, the left one also as a badge (bg). */
  void field(const cJSON *obj, const char *name, const std::string &path, bool may_icon = false) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (!v || cJSON_IsNull(v))
      return;
    const std::string p = member(path, name);
    if (!object(v, p))
      return;
    if (may_icon)
      allowed(v, p, {"item", "format", "factor", "text", "icon", "color", "bg", "value_style"});
    else
      allowed(v, p, {"item", "format", "factor", "text", "color", "value_style"});
    rules(v, p);
    if (cJSON_GetObjectItem(v, "value_style") && !cJSON_GetObjectItem(v, "item"))
      fail(member(p, "value_style"), "needs an \"item\" (it follows the item's state)");
    const cJSON *item = cJSON_GetObjectItem(v, "item");
    const cJSON *text = cJSON_GetObjectItem(v, "text");
    const cJSON *glyph = cJSON_GetObjectItem(v, "icon");
    if ((item != nullptr) + (text != nullptr) + (glyph != nullptr) != 1)
      fail(p, may_icon ? "needs one of \"item\", \"text\" or \"icon\"" : "needs either \"item\" or \"text\"");
    if (glyph)
      icon(v, p);
    color(v, p);
    if (const cJSON *bg = cJSON_GetObjectItem(v, "bg"))
      color_value(bg, member(p, "bg"));
    if (item)
      item_name(item, member(p, "item"));
    if (text)
      string(v, "text", p, false, 64);
    format(v, p);
  }

  /* An array of exactly `count` strings. */
  void strings(const cJSON *obj, const char *name, const std::string &path, int count) {
    const cJSON *v = cJSON_GetObjectItem(obj, name);
    if (!v)
      return;
    bool ok = cJSON_IsArray(v) && cJSON_GetArraySize(v) == count;
    for (const cJSON *e = ok ? v->child : nullptr; e; e = e->next)
      ok = ok && cJSON_IsString(e) && std::strlen(e->valuestring) <= 32;
    if (!ok)
      fail(member(path, name), "must be an array of " + std::to_string(count) + " strings");
  }

  void display(const cJSON *d, const std::string &path) {
    if (!object(d, path))
      return;
    allowed(d, path, {"language", "date_format", "time_format", "background", "theme", "texts", "home_after", "screen",
                      "notifications", "page_fade", "cover_reveal"});
    // how the media page's cover appears (Cover.h)
    one_of(d, "cover_reveal", path, {"dissolve", "center", "sharpen", "wipe"});
    // a swipe to another page fades out and in, that many ms each way (0: not)
    if (const cJSON *f = cJSON_GetObjectItem(d, "page_fade"))
      if (!cJSON_IsNumber(f) || f->valueint < 0 || f->valueint > 1000)
        fail(member(path, "page_fade"), "must be 0 (off) to 1000 (ms)");
    // a String item whose new states show as notifications: "level|title|text"
    if (const cJSON *n = cJSON_GetObjectItem(d, "notifications")) {
      const std::string p = member(path, "notifications");
      if (object(n, p)) {
        allowed(n, p, {"item"});
        item_name(cJSON_GetObjectItem(n, "item"), member(p, "item"));
      }
    }
    if (const cJSON *screen = cJSON_GetObjectItem(d, "screen"))
      screen_settings(screen, member(path, "screen"));
    // back to home after that many seconds without a touch on another page; 0: never
    if (const cJSON *h = cJSON_GetObjectItem(d, "home_after"))
      if (!cJSON_IsNumber(h) || h->valuedouble != h->valueint || h->valueint < 0 || h->valueint > 3600)
        fail(member(path, "home_after"), "must be 0 (never) to 3600 (seconds)");
    if (const cJSON *theme = cJSON_GetObjectItem(d, "theme")) {
      const std::string p = member(path, "theme");
      if (object(theme, p)) {
        allowed(theme, p, {"accent", "text", "dim"});
        for (const char *k : {"accent", "text", "dim"})
          if (const cJSON *c = cJSON_GetObjectItem(theme, k))
            color_value(c, member(p, k), false);   // a theme colour of its own: no placeholder
      }
    }
    /* Which ids exist is decided by the display design (boards/nspanel/nextion/display-manifest.json); the display
       keeps its default background for one it does not know. */
    if (const cJSON *bg = cJSON_GetObjectItem(d, "background"))
      if (!cJSON_IsString(bg) || !valid_slug(bg->valuestring))
        fail(member(path, "background"), "must be a background id such as \"aurora\"");
    // texts: begin
    one_of(d, "language", path, {"en", "de", "fr", "es", "it", "nl", "pt", "da", "sv", "nb", "fi", "bg", "cs", "el", "et", "ga", "hr", "hu", "lt", "lv", "mt", "pl", "ro", "sk", "sl"});
    // texts: end
    string(d, "date_format", path, false, 48);
    string(d, "time_format", path, false, 16);
    const cJSON *texts = cJSON_GetObjectItem(d, "texts");
    if (!texts)
      return;
    const std::string p = member(path, "texts");
    if (!object(texts, p))
      return;
    allowed(texts, p, {"on", "off", "undefined", "weekdays", "weekdays_short", "months", "months_short",
                       "settings_title", "settings", "never", "restart", "restart_confirm"});
    for (const char *n : {"on", "off", "undefined", "never"})
      string(texts, n, p, false, 16);
    for (const char *n : {"settings_title", "restart", "restart_confirm"})
      string(texts, n, p, false, 32);
    strings(texts, "settings", p, 3);
    strings(texts, "weekdays", p, 7);
    strings(texts, "weekdays_short", p, 7);
    strings(texts, "months", p, 12);
    strings(texts, "months_short", p, 12);
  }

  void optional_item(const cJSON *obj, const std::string &path) {
    if (const cJSON *v = cJSON_GetObjectItem(obj, "item"))
      item_name(v, member(path, "item"));
  }

  static bool icon_name(const cJSON *v) {
    bool ok = cJSON_IsString(v) && *v->valuestring && std::strlen(v->valuestring) <= 64;
    for (const char *s = ok ? v->valuestring : ""; *s; s++)
      ok = ok && ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-');
    return ok;
  }

  /* `animated`: a list of 2 to 8 icons shown in turn is allowed too, with an
     "interval" (ms). */
  void icon(const cJSON *obj, const std::string &path, bool required = false, bool animated = false) {
    const cJSON *v = cJSON_GetObjectItem(obj, "icon");
    if (!v) {
      if (required)
        fail(member(path, "icon"), "is required");
      return;
    }
    if (animated && cJSON_IsArray(v)) {
      const int n = cJSON_GetArraySize(v);
      bool ok = n >= 2 && n <= 8;
      for (const cJSON *e = v->child; e && ok; e = e->next)
        ok = icon_name(e);
      if (!ok)
        fail(member(path, "icon"), "must be an icon name, or a list of 2 to 8 shown in turn");
    } else if (!icon_name(v)) {
      fail(member(path, "icon"), "must be a Material Design Icons name such as \"desk-lamp\"");
    }
    if (const cJSON *iv = cJSON_GetObjectItem(obj, "interval")) {
      if (!animated)
        fail(member(path, "interval"), "is not allowed here");
      else if (!cJSON_IsNumber(iv) || iv->valueint < 200 || iv->valueint > 5000)
        fail(member(path, "interval"), "must be 200 to 5000 (ms)");
    }
  }

  void color_value(const cJSON *v, const std::string &path, bool theme = true) {
    if (cJSON_IsString(v)) {
      const char *s = v->valuestring;
      for (const char *c : kColors)
        if (std::strcmp(s, c) == 0)
          return;
      for (const char *c : kThemeColors)
        if (theme && std::strcmp(s, c) == 0)
          return;
      bool hex = s[0] == '#' && std::strlen(s) == 7;
      for (int i = 1; hex && i < 7; i++)
        hex = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F');
      if (hex)
        return;
    }
    fail(path, theme ? "must be a palette color (white, black, red, green, blue, yellow, brown, grey, light-grey, "
                       "dark-grey), a theme color (theme-accent, theme-text, theme-dim) or #rrggbb"
                     : "must be a palette color (white, black, red, green, blue, yellow, brown, grey, light-grey, "
                       "dark-grey) or #rrggbb");
  }

  void color(const cJSON *obj, const std::string &path, const char *key = "color") {
    if (const cJSON *v = cJSON_GetObjectItem(obj, key))
      color_value(v, member(path, key));
  }

  void page_ref(const cJSON *v, const std::string &path) {
    if (!cJSON_IsString(v))
      fail(path, "must be a page id");
    else if (!ids_.count(v->valuestring) && std::strcmp(v->valuestring, kSettingsId) != 0 &&
             std::strcmp(v->valuestring, kInfoId) != 0)
      fail(path, "no page with id \"" + std::string(v->valuestring) + "\"");
  }

  void open(const cJSON *obj, const std::string &path) {
    if (const cJSON *v = cJSON_GetObjectItem(obj, "open"))
      page_ref(v, member(path, "open"));
  }

  /* `button`: a button's rules, which may also set "active" (the tile drawn active or not). */
  void rules(const cJSON *obj, const std::string &path, bool button = false, bool lock = false) {
    const cJSON *list = cJSON_GetObjectItem(obj, "value_style");
    if (!list)
      return;
    const std::string p = member(path, "value_style");
    if (!cJSON_IsArray(list)) {
      fail(p, "must be an array of rules");
      return;
    }
    int index = 0;
    for (const cJSON *r = list->child; r; r = r->next, index++) {
      const std::string rp = at(p, index);
      if (!object(r, rp))
        continue;
      if (lock)   // a thermostat's mode: whether its target may be set
        allowed(r, rp, {"below", "above", "at_most", "at_least", "equals", "locked"});
      else if (button)
        allowed(r, rp, {"below", "above", "at_most", "at_least", "equals", "within_days", "from", "until", "color",
                        "blink", "icon", "active", "layout"});
      else
        allowed(r, rp, {"below", "above", "at_most", "at_least", "equals", "within_days", "from", "until", "color",
                        "blink", "icon", "icon_color", "format", "factor"});
      // what it sets: a colour, blinking (ms on and off), another icon or its colour, another format (with its
      // factor) -- a button: whether it is drawn active
      if (!cJSON_GetObjectItem(r, "color") && !cJSON_GetObjectItem(r, "blink") && !cJSON_GetObjectItem(r, "icon") &&
          !cJSON_GetObjectItem(r, "icon_color") && !cJSON_GetObjectItem(r, "format") && !cJSON_GetObjectItem(r, "active") &&
          !cJSON_GetObjectItem(r, "layout") && !cJSON_GetObjectItem(r, "locked"))
        fail(rp, lock ? "sets nothing: give \"locked\"" : button ? "sets nothing: give \"active\", \"layout\", \"color\", \"blink\" or \"icon\""
                        : "sets nothing: give \"color\", \"blink\", \"icon\", \"icon_color\" or \"format\"");
      boolean(r, "active", rp);
      boolean(r, "locked", rp);
      one_of(r, "layout", rp, {"corner", "center"});   // a button's icon place while it matches
      if (const cJSON *c = cJSON_GetObjectItem(r, "icon_color"))
        color_value(c, member(rp, "icon_color"));
      format(r, rp);
      if (cJSON_GetObjectItem(r, "factor") && !cJSON_GetObjectItem(r, "format"))
        fail(member(rp, "factor"), "goes with a \"format\" of the rule");
      if (const cJSON *b = cJSON_GetObjectItem(r, "blink"))
        if (!cJSON_IsNumber(b) || b->valuedouble != b->valueint || b->valueint < 200 || b->valueint > 2000)
          fail(member(rp, "blink"), "must be 200 to 2000 (ms)");
      if (cJSON_GetObjectItem(r, "icon"))
        icon(r, rp);
      int conditions = 0;
      for (const char *num : {"below", "above", "at_most", "at_least", "within_days"}) {
        if (const cJSON *v = cJSON_GetObjectItem(r, num)) {
          conditions++;
          // a threshold may be a group member's metadata ("@warn", entity pages with "metadata")
          const bool ref = num[0] != 'w' && cJSON_IsString(v) && v->valuestring[0] == '@' && v->valuestring[1];
          if (!cJSON_IsNumber(v) && !ref)
            fail(member(rp, num), num[0] == 'w' ? "must be a number" : "must be a number or \"@key\" (metadata)");
        }
      }
      // a date rule's times of day: the window's first day from, the date's own day until
      for (const char *t : {"from", "until"})
        if (const cJSON *v = cJSON_GetObjectItem(r, t)) {
          if (!cJSON_GetObjectItem(r, "within_days"))
            fail(member(rp, t), "goes with \"within_days\"");
          else if (!cJSON_IsString(v) || !clock_time(v->valuestring))
            fail(member(rp, t), "must be a time, \"HH:MM\"");
        }
      if (const cJSON *v = cJSON_GetObjectItem(r, "equals")) {
        conditions++;
        if (!cJSON_IsString(v))
          fail(member(rp, "equals"), "must be a string");
      }
      if (conditions > 1)
        fail(rp, "one condition per rule");
      if (conditions == 0 && r->next)
        fail(rp, "a rule without a condition always matches, so it must be the last one");
      if (const cJSON *c = cJSON_GetObjectItem(r, "color"))
        color_value(c, member(rp, "color"));
    }
  }

  /* An array with at most `max` entries, each an object or null. */
  template <typename Fn>
  void slots(const cJSON *obj, const char *name, const std::string &path, int max, Fn each) {
    const cJSON *list = cJSON_GetObjectItem(obj, name);
    if (!list)
      return;
    const std::string p = member(path, name);
    if (!cJSON_IsArray(list)) {
      fail(p, "must be an array");
      return;
    }
    if (cJSON_GetArraySize(list) > max)
      fail(p, "at most " + std::to_string(max) + " entries");
    int index = 0;
    for (const cJSON *e = list->child; e; e = e->next, index++) {
      if (cJSON_IsNull(e))
        continue;
      if (object(e, at(p, index)))
        each(e, at(p, index));
    }
  }

  /* "on": the state that counts as on, or a list of them. */
  void on_states(const cJSON *e, const std::string &path) {
    const cJSON *v = cJSON_GetObjectItem(e, "on");
    if (!v)
      return;
    const std::string p = member(path, "on");
    const auto ok = [](const cJSON *s) {
      return cJSON_IsString(s) && s->valuestring[0] && std::strlen(s->valuestring) <= 32;
    };
    if (cJSON_IsArray(v)) {
      const int n = cJSON_GetArraySize(v);
      bool all = n >= 1 && n <= 8;
      for (const cJSON *s = v->child; s && all; s = s->next)
        all = ok(s);
      if (!all)
        fail(p, "must be a state or a list of 1 to 8 states (up to 32 characters each)");
    } else if (!ok(v)) {
      fail(p, "must be a state (up to 32 characters) or a list of them");
    }
  }

  void indicator(const cJSON *e, const std::string &p, bool may_open) {
    if (may_open)
      allowed(e, p, {"item", "icon", "interval", "color", "show", "on", "open", "open_when_off", "blink",
                     "value_style"});
    else
      allowed(e, p, {"item", "icon", "interval", "color", "show", "on", "blink", "value_style"});
    rules(e, p);
    optional_item(e, p);
    on_states(e, p);
    icon(e, p, true, true);
    // "blink": the icon on and off, ms each -- one icon (several are shown in turn already)
    if (const cJSON *b = cJSON_GetObjectItem(e, "blink")) {
      if (!cJSON_IsNumber(b) || b->valuedouble != b->valueint || b->valueint < 200 || b->valueint > 2000)
        fail(member(p, "blink"), "must be 200 to 2000 (ms)");
      else if (cJSON_IsArray(cJSON_GetObjectItem(e, "icon")))
        fail(member(p, "blink"), "only with one icon (several are shown in turn already)");
    }
    color(e, p);
    one_of(e, "show", p, {"always", "when_on"});
    open(e, p);
    boolean(e, "open_when_off", p);   // a shortcut's page opens while its item is off too
  }

  void home(const cJSON *h, const std::string &path) {
    if (!object(h, path))
      return;
    allowed(h, path,
            {"relays", "values", "indoor_temperature", "outdoor_temperature", "outdoor_high", "outdoor_low",
             "weather_icon", "weather_page",
             "status_icons", "shortcuts", "info"});
    slots(h, "relays", path, kMaxRelays, [&](const cJSON *e, const std::string &p) {
      allowed(e, p, {"label", "item", "relay"});
      string(e, "label", p, false, 24);
      optional_item(e, p);
      boolean(e, "relay", p);
    });
    item_ref(h, "indoor_temperature", path);  // older name of the first value row
    slots(h, "values", path, kMaxValueRows, [&](const cJSON *e, const std::string &p) {
      allowed(e, p, {"item", "icon", "color", "format", "factor", "decimals", "value_style"});
      item_name(cJSON_GetObjectItem(e, "item"), member(p, "item"));
      rules(e, p);
      decimals(e, p);
      icon(e, p);
      color(e, p);
      format(e, p);
    });
    item_ref(h, "outdoor_temperature", path);
    item_ref(h, "outdoor_high", path);  // today's, under it
    item_ref(h, "outdoor_low", path);
    item_ref(h, "weather_icon", path, false, true);
    if (const cJSON *v = cJSON_GetObjectItem(h, "weather_page"))
      page_ref(v, member(path, "weather_page"));
    slots(h, "status_icons", path, kMaxStatusIcons, [&](const cJSON *e, const std::string &p) { indicator(e, p, false); });
    slots(h, "shortcuts", path, kMaxShortcuts, [&](const cJSON *e, const std::string &p) { indicator(e, p, true); });
    slots(h, "info", path, kMaxInfoLines, [&](const cJSON *e, const std::string &p) {
      allowed(e, p, {"left", "main", "right"});
      field(e, "left", p, true);
      field(e, "main", p);
      field(e, "right", p);
    });
  }

  void button(const cJSON *b, const std::string &p) {
    allowed(b, p, {"label", "icon", "color", "item", "action", "command", "open", "value", "always_active", "layout",
                   "value_style"});
    rules(b, p, true);   // active (or not), a colour, blinking, another icon -- by the item's state
    if (cJSON_GetObjectItem(b, "value_style") && !cJSON_GetObjectItem(b, "item"))
      fail(member(p, "value_style"), "needs an \"item\" (it follows the item's state)");
    one_of(b, "layout", p, {"corner", "center"});   // the icon top left with the small line, or in the middle alone
    string(b, "label", p, false, 32);
    icon(b, p, true);
    color(b, p);
    optional_item(b, p);
    one_of(b, "action", p, {"toggle", "command", "open", "none"});
    string(b, "command", p, false, 64);
    open(b, p);
    one_of(b, "value", p, {"on_off", "percent", "none"});
    boolean(b, "always_active", p);

    const cJSON *action = cJSON_GetObjectItem(b, "action");
    const char *a = cJSON_IsString(action) ? action->valuestring : "";
    const bool has_item = cJSON_GetObjectItem(b, "item") != nullptr;
    if ((std::strcmp(a, "toggle") == 0 || std::strcmp(a, "command") == 0) && !has_item)
      fail(member(p, "item"), std::string("is required for action \"") + a + "\"");
    if (std::strcmp(a, "command") == 0 && !cJSON_GetObjectItem(b, "command"))
      fail(member(p, "command"), "is required for action \"command\"");
    if (std::strcmp(a, "open") == 0 && !cJSON_GetObjectItem(b, "open"))
      fail(member(p, "open"), "is required for action \"open\"");
  }

  void row(const cJSON *r, const std::string &p) {
    allowed(r, p, {"item", "label", "icon", "color", "value_color", "value_style", "format", "factor", "wide",
                   "scroll"});
    boolean(r, "scroll", p);   // a text too long for its field scrolls (else the page's "scroll" counts)
    color(r, p, "value_color");   // the value's colour without a rule (a rule's colour comes first)
    boolean(r, "wide", p);   // this row in the wide layout (a day's date over its lessons, say)
    const cJSON *item = cJSON_GetObjectItem(r, "item");
    if (!item)
      fail(member(p, "item"), "is required");
    else
      item_name(item, member(p, "item"));
    string(r, "label", p, false, 64);
    icon(r, p);
    color(r, p);
    rules(r, p);
    format(r, p);
  }

  void page(const cJSON *pg, const std::string &p) {
    if (!object(pg, p))
      return;
    const cJSON *id = cJSON_GetObjectItem(pg, "id");
    if (!cJSON_IsString(id) || !valid_slug(id->valuestring))
      fail(member(p, "id"), "must be 1-24 characters a-z 0-9 - (not starting or ending with -)");
    one_of(pg, "type", p,
           {kPageTypes[0], kPageTypes[1], kPageTypes[2], kPageTypes[3], kPageTypes[4], kPageTypes[5], kPageTypes[6],
            kPageTypes[7], kPageTypes[8], kPageTypes[9], kPageTypes[10]},
           true);
    const cJSON *type = cJSON_GetObjectItem(pg, "type");
    const std::string t = cJSON_IsString(type) ? type->valuestring : "";
    // the info page has a title of its own in the display language
    // a cover may leave it out: its item's metadata's label then
    string(pg, "title", p, t != "system" && !(t == "cover" && cJSON_GetObjectItem(pg, "item")), 64);

    if (t == "buttons") {
      allowed(pg, p, {"id", "type", "title", "buttons"});
      slots(pg, "buttons", p, kMaxButtons, [&](const cJSON *b, const std::string &bp) { button(b, bp); });
    } else if (t == "entities") {
      allowed(pg, p, {"id", "type", "title", "rows", "group", "icon", "color", "value_style", "format", "factor",
                       "wide", "style", "metadata", "label", "value_color", "scroll"});
      const bool has_rows = cJSON_GetObjectItem(pg, "rows") != nullptr;
      const cJSON *group = cJSON_GetObjectItem(pg, "group");
      if (has_rows == (group != nullptr))
        fail(p, "needs either \"rows\" or \"group\"");
      if (group)
        item_name(group, member(p, "group"));
      slots(pg, "rows", p, kMaxRows, [&](const cJSON *r, const std::string &rp) { row(r, rp); });
      icon(pg, p);
      color(pg, p);
      color(pg, p, "value_color");
      rules(pg, p);
      format(pg, p);
      boolean(pg, "wide", p);
      boolean(pg, "scroll", p);   // texts too long for their fields scroll (the list and the rows)
      one_of(pg, "style", p, {"list", "cards", "rows"});
      string(pg, "metadata", p, false, 64);   // a namespace of the members' metadata
      string(pg, "label", p, false, 64);      // a template: {value}, {key} of that metadata, {label}
      if (!group && (cJSON_GetObjectItem(pg, "metadata") || cJSON_GetObjectItem(pg, "label")))
        fail(p, "\"metadata\" and \"label\" are for \"group\"");
      if (has_rows && (cJSON_GetObjectItem(pg, "icon") || cJSON_GetObjectItem(pg, "value_style")))
        fail(p, "\"icon\", \"color\", \"value_style\" and \"format\" on the page are for \"group\"; give them per row");
    } else if (t == "cover") {
      allowed(pg, p, {"id", "type", "title", "icon", "item", "group", "hide_slider", "commands", "live", "style",
                      "reverse", "presets", "moving", "metadata", "from"});
      one_of(pg, "from", p, {"left", "right"});   // a gate: the side it comes in from (default right)
      boolean(pg, "live", p);   // the position sent while the finger moves on the drawing (default: when let go)
      icon(pg, p);
      // one cover ("item") or a group's members as rows ("group", its own commands to all of them)
      const cJSON *item = cJSON_GetObjectItem(pg, "item"), *group = cJSON_GetObjectItem(pg, "group");
      if (!item && !group)
        fail(member(p, "item"), "is required (or \"group\")");
      else if (item && group)
        fail(member(p, "group"), "goes without \"item\"");
      if (item)
        item_name(item, member(p, "item"));
      if (group)
        item_name(group, member(p, "group"));
      one_of(pg, "style", p, {"shutter", "screen", "awning", "curtain", "garage", "gate"});
      boolean(pg, "reverse", p);   // the item's 0 is closed / extended
      string(pg, "metadata", p, false, 64);   // the namespace of the items' metadata (default "cover")
      item_ref(pg, "moving", p);   // UP / DOWN / STOP while it moves
      if (const cJSON *pr = cJSON_GetObjectItem(pg, "presets")) {
        const std::string pp = member(p, "presets");
        if (!cJSON_IsArray(pr) || cJSON_GetArraySize(pr) > 3) {
          fail(pp, "must be a list of up to 3 presets");
        } else {
          int k = 0;
          for (const cJSON *e = pr->child; e; e = e->next, k++) {
            const std::string ep = pp + "[" + std::to_string(k) + "]";
            if (!object(e, ep))
              continue;
            allowed(e, ep, {"label", "position"});
            string(e, "label", ep, true, 16);
            const cJSON *pos = cJSON_GetObjectItem(e, "position");
            if (!cJSON_IsNumber(pos) || pos->valuedouble < 0 || pos->valuedouble > 100)
              fail(member(ep, "position"), "must be a number 0 to 100");
          }
        }
      }
      boolean(pg, "hide_slider", p);
      if (const cJSON *c = cJSON_GetObjectItem(pg, "commands")) {
        const std::string cp = member(p, "commands");
        if (object(c, cp)) {
          allowed(c, cp, {"item", "stop", "open", "close"});
          if (const cJSON *ci = cJSON_GetObjectItem(c, "item"))   // up / stop / down to this item, not "item"
            item_name(ci, member(cp, "item"));
          string(c, "stop", cp, false, 32);
          string(c, "open", cp, false, 32);
          string(c, "close", cp, false, 32);
        }
      }
    } else if (t == "thermostat") {
      allowed(pg, p, {"id", "type", "title", "icon", "item", "current", "heating", "cooling", "mode", "modes", "min",
                      "max", "step", "style", "curve"});
      // the gradient along the ring: the share's power (below 1 red the sooner)
      if (const cJSON *c = cJSON_GetObjectItem(pg, "curve")) {
        const double v = cJSON_IsNumber(c) ? c->valuedouble : -1;
        if (v != 0.25 && v != 0.5 && v != 0.75 && v != 1 && v != 1.5 && v != 2)
          fail(member(p, "curve"), "must be 0.25, 0.5, 0.75, 1, 1.5 or 2");
      }
      icon(pg, p);
      // the target temperature (a Number, or Number:Temperature): what the ring and minus / plus set
      if (const cJSON *item = cJSON_GetObjectItem(pg, "item"))
        item_name(item, member(p, "item"));
      else
        fail(member(p, "item"), "is required (the target temperature)");
      item_ref(pg, "current", p);   // the temperature now
      item_ref(pg, "heating", p);   // ON / OPEN / above 0 while it heats
      item_ref(pg, "cooling", p);   // ... cools
      // its mode, chosen from "modes" (else the item's options in openHAB); its rules: in which the target is locked
      if (const cJSON *m = cJSON_GetObjectItem(pg, "mode"); cJSON_IsObject(m)) {
        allowed(m, member(p, "mode"), {"item", "value_style"});
        item_name(cJSON_GetObjectItem(m, "item"), member(member(p, "mode"), "item"));
        rules(m, member(p, "mode"), false, true);
      } else if (m) {
        fail(member(p, "mode"), "must be an object");
      }
      one_of(pg, "style", p, {"accent", "gradient"});   // the ring filled in the accent (default) or blue to red
      for (const char *k : {"min", "max", "step"})   // the target's (default: the item's in openHAB, else 5, 30, 0.5)
        if (const cJSON *v = cJSON_GetObjectItem(pg, k); v && (!cJSON_IsNumber(v) || v->valuedouble < -100 ||
                                                               v->valuedouble > 1000))
          fail(member(p, k), "must be a number (-100 .. 1000)");
      if (const cJSON *v = cJSON_GetObjectItem(pg, "step"); cJSON_IsNumber(v) && v->valuedouble <= 0)
        fail(member(p, "step"), "must be above 0");
      const cJSON *lo = cJSON_GetObjectItem(pg, "min"), *hi = cJSON_GetObjectItem(pg, "max");
      if (cJSON_IsNumber(lo) && cJSON_IsNumber(hi) && hi->valuedouble <= lo->valuedouble)
        fail(member(p, "max"), "must be above \"min\"");
      if (const cJSON *ms = cJSON_GetObjectItem(pg, "modes")) {
        const std::string mp = member(p, "modes");
        if (!cJSON_IsArray(ms) || cJSON_GetArraySize(ms) > 4) {
          fail(mp, "must be a list of up to 4 modes");
        } else {
          if (!cJSON_GetObjectItem(pg, "mode"))
            fail(mp, "goes with \"mode\" (the item they are sent to)");
          int k = 0;
          for (const cJSON *e = ms->child; e; e = e->next, k++) {
            const std::string ep = mp + "[" + std::to_string(k) + "]";
            if (!object(e, ep))
              continue;
            allowed(e, ep, {"label", "value"});
            string(e, "label", ep, false, 16);
            string(e, "value", ep, true, 32);
          }
        }
      }
    } else if (t == "keypad") {
      allowed(pg, p, {"id", "type", "title", "icon", "color", "text", "item", "state", "result", "length", "keys",
                      "buttons"});
      // the line over the code without a state: its words, icon and colour
      string(pg, "text", p, false, 32);
      icon(pg, p);
      color(pg, p);
      if (const cJSON *item = cJSON_GetObjectItem(pg, "item"))   // the buttons' item, unless one names its own
        item_name(item, member(p, "item"));
      // the state over the code: its item's, written and coloured by its rules (a text, an icon, blinking)
      if (const cJSON *s = cJSON_GetObjectItem(pg, "state")) {
        const std::string sp = member(p, "state");
        if (object(s, sp)) {
          allowed(s, sp, {"item", "format", "factor", "decimals", "icon", "color", "value_style"});
          item_name(cJSON_GetObjectItem(s, "item"), member(sp, "item"));
          decimals(s, sp);
          format(s, sp);
          icon(s, sp);
          color(s, sp);
          rules(s, sp);
        }
      }
      item_ref(pg, "result", p);   // a message after the code went: its new state, shown a while
      if (const cJSON *n = cJSON_GetObjectItem(pg, "length"); n && (!cJSON_IsNumber(n) || n->valuedouble < 1 ||
                                                                    n->valuedouble > 8 ||
                                                                    n->valuedouble != n->valueint))
        fail(member(p, "length"), "must be a whole number 1 to 8 (the code's places)");
      one_of(pg, "keys", p, {"digits", "phone"});   // the bottom row: backspace, 0, C -- or *, 0, #
      const cJSON *bs = cJSON_GetObjectItem(pg, "buttons");
      const std::string bp = member(p, "buttons");
      if (!cJSON_IsArray(bs) || cJSON_GetArraySize(bs) < 1 || cJSON_GetArraySize(bs) > 4) {
        fail(bp, "must be a list of 1 to 4 buttons");
      } else {
        int k = 0;
        for (const cJSON *e = bs->child; e; e = e->next, k++) {
          const std::string ep = bp + "[" + std::to_string(k) + "]";
          if (!object(e, ep))
            continue;
          allowed(e, ep, {"label", "icon", "item", "command", "code", "when", "show_when"});
          string(e, "label", ep, true, 16);
          icon(e, ep);
          if (const cJSON *item = cJSON_GetObjectItem(e, "item"))
            item_name(item, member(ep, "item"));
          else if (!cJSON_GetObjectItem(pg, "item"))
            fail(member(ep, "item"), "is required (or the page's \"item\")");
          string(e, "command", ep, false, 64);   // {code}: the code typed (default: the code alone)
          boolean(e, "code", ep);   // false: works without a code too
          string(e, "when", ep, false, 32);   // the state it is lit in
          // the states it shows in: one, or a list of them (else always)
          if (const cJSON *sw = cJSON_GetObjectItem(e, "show_when")) {
            bool ok = cJSON_IsString(sw) || (cJSON_IsArray(sw) && cJSON_GetArraySize(sw) >= 1);
            for (const cJSON *s = cJSON_IsArray(sw) ? sw->child : nullptr; s; s = s->next)
              ok = ok && cJSON_IsString(s);
            if (!ok)
              fail(member(ep, "show_when"), "must be a state, or a list of states");
            else if (!cJSON_GetObjectItem(pg, "state"))
              fail(member(ep, "show_when"), "goes with the page's \"state\" (the item whose state it is)");
          }
        }
      }
    } else if (t == "light") {
      allowed(pg, p, {"id", "type", "title", "icon", "brightness", "color", "color_temperature", "min", "max",
                      "reverse", "live", "unit", "switch"});
      item_ref(pg, "switch", p);   // what the icon switches (default: the brightness, with ON / OFF)
      // the colour temperature's unit (default: kelvin with an item in K, else percent) and its ends in it
      one_of(pg, "unit", p, {"kelvin", "mired", "percent"});
      boolean(pg, "live", p);   // false: the sliders sent only when let go
      boolean(pg, "reverse", p);   // a colour temperature in %: 0 is cold (the slider's top) instead of warm
      for (const char *k : {"min", "max"})   // the colour temperature slider's ends, in its unit
        if (const cJSON *v = cJSON_GetObjectItem(pg, k); v && (!cJSON_IsNumber(v) || v->valuedouble < 0 ||
                                                               v->valuedouble > 20000))
          fail(member(p, k), "must be a number of the colour temperature's unit (0 .. 20000)");
      icon(pg, p);
      item_ref(pg, "brightness", p, true);
      item_ref(pg, "color", p);
      item_ref(pg, "color_temperature", p);
    } else if (t == "media") {
      allowed(pg, p, {"id", "type", "title", "icon", "control", "mute", "volume", "track", "artist", "album", "art",
                      "favorites", "play", "uri", "live"});
      boolean(pg, "live", p);   // false: the volume sent only when the slider is let go
      icon(pg, p);
      item_ref(pg, "control", p, true);
      // art: what plays as a picture -- an item holding its address (or the picture as a data: address)
      // uri: what plays, as an address (Sonos: currenttrackuri) -- a favourite's name shows again by it
      for (const char *n : {"mute", "volume", "track", "artist", "album", "art", "uri"})
        item_ref(pg, n, p);
      // the favourites page its star button opens, and the item a favourite's stream goes to
      if (const cJSON *f = cJSON_GetObjectItem(pg, "favorites")) {
        page_ref(f, member(p, "favorites"));
        if (!cJSON_GetObjectItem(pg, "play"))
          fail(member(p, "play"), "is required with \"favorites\": the item a favourite's stream goes to");
      }
      item_ref(pg, "play", p);
    } else if (t == "media_favorites") {
      // a station catalogue: an item whose options are "stream URI=key" (its "ohpanel" metadata gives
      // names, icons and logos); the player it plays on when not opened from one
      allowed(pg, p, {"id", "type", "title", "stations", "player"});
      const cJSON *st = cJSON_GetObjectItem(pg, "stations");
      const std::string sp = member(p, "stations");
      if (!st)
        fail(sp, "is required");
      else if (object(st, sp)) {
        allowed(st, sp, {"item"});
        item_name(cJSON_GetObjectItem(st, "item"), member(sp, "item"));
      }
      if (const cJSON *pl = cJSON_GetObjectItem(pg, "player"))
        page_ref(pl, member(p, "player"));
    } else if (t == "weather") {
      // the days of the forecast as sheets ("days"), or one day on the page itself (as before "days" came)
      const auto one_day = [&](const cJSON *d, const std::string &dp) {
        const cJSON *day = cJSON_GetObjectItem(d, "day");
        if (!cJSON_IsNumber(day) || day->valueint < 0 || day->valueint > 6 || day->valuedouble != day->valueint)
          fail(member(dp, "day"), "must be 0 (today) to 6");
        item_ref(d, "icon", dp, true, true);
        item_ref(d, "min", dp);
        item_ref(d, "max", dp);
        item_ref(d, "condition", dp);   // in words: "light rain"
        slots(d, "extras", dp, kMaxExtras, [&](const cJSON *e, const std::string &ep) {
          allowed(e, ep, {"icon", "item", "label", "color"});
          icon(e, ep, true);
          item_name(cJSON_GetObjectItem(e, "item"), member(ep, "item"));
          string(e, "label", ep, false, 16);
          color(e, ep);
        });
      };
      if (const cJSON *days = cJSON_GetObjectItem(pg, "days")) {
        allowed(pg, p, {"id", "type", "title", "days"});
        const std::string dp = member(p, "days");
        if (!cJSON_IsArray(days) || cJSON_GetArraySize(days) < 1 || cJSON_GetArraySize(days) > kMaxDays) {
          fail(dp, "must be a list of 1 to 7 days");
        } else {
          int k = 0;
          for (const cJSON *d = days->child; d; d = d->next, k++) {
            const std::string kp = at(dp, k);
            if (!object(d, kp))
              continue;
            allowed(d, kp, {"title", "day", "icon", "min", "max", "condition", "extras"});
            string(d, "title", kp, false, 24);
            one_day(d, kp);
          }
        }
      } else {
        allowed(pg, p, {"id", "type", "title", "day", "icon", "min", "max", "condition", "extras"});
        one_day(pg, p);
      }
    } else if (t == "qr") {
      allowed(pg, p, {"id", "type", "title", "text", "caption", "color", "style", "ecc"});
      string(pg, "text", p, true, 200);  // what fits the card (Engine::draw_qr_code: up to version 10)
      // one line, or two as a list (caption1, caption2: txt_maxl 80 in the HMI)
      if (const cJSON *c = cJSON_GetObjectItem(pg, "caption"); cJSON_IsArray(c)) {
        if (cJSON_GetArraySize(c) > 2)
          fail(member(p, "caption"), "at most 2 lines");
        int k = 0;
        for (const cJSON *l = c->child; l; l = l->next, k++)
          if (!cJSON_IsString(l) || std::strlen(l->valuestring) > 80)
            fail(member(p, "caption") + "[" + std::to_string(k) + "]", "must be a text of up to 80 characters");
      } else {
        string(pg, "caption", p, false, 80);
      }
      color(pg, p);
      one_of(pg, "style", p, {"solid", "dots"});
      one_of(pg, "ecc", p, {"low", "medium", "quartile", "high"});
    } else if (t == "system") {
      allowed(pg, p, {"id", "type", "title"});
    }
  }
};

}  // namespace

bool validate(const std::string &json, std::vector<Issue> &issues) {
  issues.clear();
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  if (!root) {
    issues.push_back({"", "not valid JSON"});
    return false;
  }
  Validator(issues).document(root);
  cJSON_Delete(root);
  return issues.empty();
}

}  // namespace panel
