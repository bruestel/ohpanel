/*
   An entity page's feed: its rows as JSON in one String item, which a rule in openHAB fills.

   As the page opens (or the screen wakes on it) the panel sends the item the command "refresh" and shows
   "Loading..." until a list comes; with "refresh" seconds it asks again while the page shows, and the list stays
   meanwhile. openHAB's autoupdate sets the item to "refresh" with the command, so the list that follows is always
   a change and reaches every panel -- one that is the same as the list drawn draws nothing anew. A rule may also
   set a list whenever it likes (without being asked). No list within "timeout" seconds: "No answer from openHAB"
   above the list the panel had.

   The item holds a list of rows, or {"rows": [...], "note": "...", "title": "..."} (the title in place of the
   page's, "Departures 14:02" say; it stays while the list is asked for anew); anything else is a text the page shows (an
   error of the rule, say). A row is {"label", "value", "icon", "color", "value_color", "wide", "scroll"} or null
   for an empty line.

   Only the page shown has a feed (feed_): the item's text, at most kMaxFeedBytes, and its rows, made anew from it
   when needed -- not one per page.
*/

#include "appdisplay/Engine.h"
#include "EngineDetail.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace appdisplay {
using namespace detail;

namespace {
constexpr size_t kMaxFeedBytes = 4096;   // the item's text: more is refused ("List too long")
constexpr int kMaxFeedRows = 40;         // more rows are left out
constexpr int kFeedTimeoutS = 15;        // "timeout" when not set
constexpr const char *kRefresh = "refresh";

/* A number as a rule's JSON has it, in the display language's decimal separator, never as an exponent: whole
   numbers without decimals, others with up to `decimals` (-1: as many as it has, up to 10 digits). */
std::string number_text(double v, char decimal, int decimals = -1) {
  char buf[40];
  if (decimals >= 0 && std::fabs(v) < 1e15)
    v = std::round(v * std::pow(10, decimals)) / std::pow(10, decimals);
  if (std::fabs(v) < 1e15 && v == std::trunc(v))
    std::snprintf(buf, sizeof buf, "%.0f", v == 0 ? 0.0 : v);   // no "-0"
  else if (decimals >= 0)
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  else
    std::snprintf(buf, sizeof buf, "%.10g", v);
  if (char *dot = std::strchr(buf, '.'))
    *dot = decimal;
  return buf;
}

/* A value of a rule's JSON as the text shown: a text as it is, a number in the decimal separator, else nothing. */
std::string value_text(const cJSON *v, char decimal) {
  if (const char *s = cJSON_GetStringValue(v))
    return s;
  return cJSON_IsNumber(v) ? number_text(v->valuedouble, decimal) : std::string();
}

/* A row the panel draws from one of the list: its own keys only, the value as the row's fixed text (a number in
   the display language's decimal separator). */
cJSON *feed_row(const cJSON *in, const cJSON *page, char decimal) {
  if (cJSON_IsNull(in))
    return cJSON_CreateNull();
  if (!cJSON_IsObject(in))
    return nullptr;
  cJSON *row = cJSON_CreateObject();
  for (const char *key : {"label", "icon", "badge", "color", "value_color"})
    if (const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(in, key)))
      cJSON_AddStringToObject(row, key, s);
  cJSON_AddStringToObject(row, "text", value_text(cJSON_GetObjectItem(in, "value"), decimal).c_str());
  for (const char *key : {"wide", "scroll"})
    if (const cJSON *b = cJSON_GetObjectItem(in, key); cJSON_IsBool(b))
      cJSON_AddBoolToObject(row, key, cJSON_IsTrue(b));
  // the page's icon and colours for a row without
  for (const char *key : {"icon", "color", "value_color"})
    if (!cJSON_GetObjectItem(row, key))
      if (const char *s = cJSON_GetStringValue(member(page, key)))
        cJSON_AddStringToObject(row, key, s);
  return row;
}

/* The panel asks for the list ("ask", true when not set) -- else a rule sets it by itself. */
bool asks(const cJSON *feed) {
  const cJSON *ask = cJSON_GetObjectItem(feed, "ask");
  return !str(feed, "item").empty() && !cJSON_IsFalse(ask);
}

/* A line of its own across the row: what the page says instead of (or above) the list. */
cJSON *note_row(const std::string &text, const char *icon, const char *colour) {
  cJSON *row = cJSON_CreateObject();
  cJSON_AddStringToObject(row, "text", text.c_str());
  cJSON_AddBoolToObject(row, "wide", true);
  cJSON_AddStringToObject(row, "icon", icon);
  if (colour)
    cJSON_AddStringToObject(row, "color", colour);
  return row;
}
}  // namespace

/* The current page, if it is an entity page with a feed. */
const cJSON *Engine::feed_page() {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  return type_of(page) == PageType::Entities && member(page, "feed") ? page : nullptr;
}

void Engine::feed_reset() {
  cJSON_Delete(feed_.view);
  feed_ = Feed{};
}

/* The item's state as the page's list -- none while not known, or while it is the "refresh" a panel asked with
   (the list drawn stays). */
std::optional<std::string> Engine::feed_state_of(const cJSON *page) const {
  appoh::ItemState st;
  const std::string item = str(member(page, "feed"), "item");
  if (item.empty() || !cb_.state || !cb_.state(item, st) || st.state == kRefresh)
    return std::nullopt;
  return st.undefined() ? std::string() : std::move(st.state);
}

/* The rows of the page shown (a feed of another page counts no rows). */
const cJSON *Engine::feed_rows(const cJSON *page) {
  if (str(page, "id") != current_)
    return nullptr;
  if (feed_.page != current_) {
    feed_reset();
    feed_.page = current_;
    if (auto raw = feed_state_of(page)) {
      feed_.raw = std::move(*raw);
      feed_.have = true;
    }
  }
  if (!feed_.view)
    feed_build(page);
  return feed_.view;
}

void Engine::feed_build(const cJSON *page) {
  cJSON_Delete(feed_.view);
  cJSON *rows = feed_.view = cJSON_CreateArray();
  if (feed_.wait == Feed::Wait::Shown || (!feed_.have && !feed_.failed)) {
    cJSON_AddItemToArray(rows, note_row(settings_.texts.feed_loading, "timer-sand", nullptr));
    return;
  }
  if (feed_.failed)
    cJSON_AddItemToArray(rows, note_row(settings_.texts.feed_no_answer, "alert-circle-outline", "yellow"));
  if (!feed_.have)
    return;
  if (feed_.raw.size() > kMaxFeedBytes) {
    feed_.title.clear();
    cJSON_AddItemToArray(rows, note_row(settings_.texts.feed_too_long, "alert-circle-outline", "yellow"));
    return;
  }
  // a list, or {"rows", "note"}; any other text is shown as it is
  const size_t start = feed_.raw.find_first_not_of(" \t\r\n");
  cJSON *doc = start != std::string::npos && (feed_.raw[start] == '[' || feed_.raw[start] == '{')
                   ? cJSON_ParseWithLength(feed_.raw.data(), feed_.raw.size())
                   : nullptr;
  const cJSON *list = cJSON_IsArray(doc) ? doc : cJSON_GetObjectItem(doc, "rows");
  // the list's own title ({"title", "rows"}), read here from the one parse; while the list is asked for anew or no
  // answer came (the returns above) the title stays with the list
  const char *title =
      cJSON_IsObject(doc) && cJSON_IsArray(list) ? cJSON_GetStringValue(cJSON_GetObjectItem(doc, "title")) : nullptr;
  feed_.title = title ? title : "";
  if (!cJSON_IsArray(list)) {
    if (!feed_.raw.empty())
      cJSON_AddItemToArray(rows, note_row(feed_.raw, "information-outline", nullptr));
    cJSON_Delete(doc);
    return;
  }
  if (const char *note = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "note")); note && *note)
    cJSON_AddItemToArray(rows, note_row(note, "information-outline", nullptr));
  int n = 0;
  for (const cJSON *in = list->child; in && n < kMaxFeedRows; in = in->next)
    if (cJSON *row = feed_row(in, page, settings_.decimal)) {
      cJSON_AddItemToArray(rows, row);
      n++;
    }
  cJSON_Delete(doc);
}

/* An entity page's title: its feed's own while it has one, else the page's. */
std::string Engine::title_of(const cJSON *page) {
  if (member(page, "feed") && feed_rows(page) && !feed_.title.empty())
    return feed_.title;
  return str(page, "title");
}

/* The page opened (or the screen woke on it): the list asked for, "Loading..." until it comes. */
void Engine::ask_feed(const cJSON *page) {
  if (feed_.asked || str(page, "id") != current_)
    return;
  feed_rows(page);   // the feed is this page's
  const cJSON *feed = member(page, "feed");
  if (!asks(feed)) {   // only what a rule sets by itself
    feed_.asked = true;
    return;
  }
  if (!cb_.request || !status_.openhab)
    return;   // asked once openHAB is there (feed_tick)
  feed_.asked = true;
  cb_.request(str(feed, "item"), kRefresh);
  feed_.wait = Feed::Wait::Shown;
  sheet_ = 0;   // "Loading..." on the first sheet, and the list from its start
  feed_.failed = false;
  feed_.since_ms = now_ms_;
  cJSON_Delete(feed_.view);
  feed_.view = nullptr;
}

/* The feed item's state came: the list drawn anew -- unless it is the one drawn already. */
void Engine::feed_state(const std::string &item) {
  const cJSON *page = feed_page();
  if (!page || feed_.page != current_ || str(member(page, "feed"), "item") != item)
    return;
  std::optional<std::string> raw = feed_state_of(page);
  if (!raw)   // "refresh": asked (by this panel or another), the list drawn stays
    return;
  const int refresh = static_cast<int>(num_of(member(page, "feed"), "refresh", 0));
  feed_.next_ms = now_ms_ + refresh * 1000LL;   // another panel's answer counts as this one's
  const bool same = feed_.have && *raw == feed_.raw && feed_.wait != Feed::Wait::Shown && !feed_.failed;
  feed_.raw = std::move(*raw);
  feed_.have = true;
  feed_.wait = Feed::Wait::None;
  feed_.failed = false;
  if (same)
    return;
  feed_redraw(page);
}

/* What time does to the feed: no answer in time, the list asked for again ("refresh"). */
void Engine::feed_tick() {
  if (feed_.page.empty())   // no page with a feed shows (only one that shows has it)
    return;
  const cJSON *page = feed_page();
  if (!page || feed_.page != current_ || shown_ != template_of(page))
    return;
  const cJSON *feed = member(page, "feed");
  if (!feed_.asked) {   // the page opened before openHAB was there
    ask_feed(page);
    if (feed_.wait == Feed::Wait::Shown)
      feed_redraw(page);
    return;
  }
  if (feed_.wait != Feed::Wait::None) {
    const int timeout = static_cast<int>(num_of(feed, "timeout", kFeedTimeoutS));
    if (now_ms_ - feed_.since_ms >= timeout * 1000LL) {
      feed_.wait = Feed::Wait::None;
      feed_.failed = true;
      feed_.next_ms = now_ms_ + static_cast<int>(num_of(feed, "refresh", 0)) * 1000LL;
      feed_redraw(page);
    }
    return;
  }
  const int refresh = static_cast<int>(num_of(feed, "refresh", 0));
  if (refresh <= 0 || now_ms_ < feed_.next_ms || !asks(feed) || !cb_.request || !status_.openhab)
    return;
  cb_.request(str(feed, "item"), kRefresh);
  feed_.wait = Feed::Wait::Quiet;   // the list stays meanwhile
  feed_.since_ms = now_ms_;
}

void Engine::feed_redraw(const cJSON *page) {
  cJSON_Delete(feed_.view);
  feed_.view = nullptr;
  if (shown_ == template_of(page))
    redraw_rows(page);
}

// ------------------------------------------------------------ weather feed --

namespace {
constexpr size_t kMaxWeatherBytes = 6144;   // the item's text: more is refused ("List too long")
constexpr int kMaxWeatherDays = 7;
constexpr int kMaxWeatherExtras = 6;

/* Days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil). */
long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

/* "2026-11-13" (anything after the day ignored) as days since 1970; -1 if it is none. */
long date_day(const char *s) {
  int y, m, d;
  if (!s || std::sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
    return -1;
  return days_from_civil(y, m, d);
}

/* A temperature of the JSON as a fixed value: a number in whole degrees with "°" after it (the fields have room for no
   more), a text as it is. */
cJSON *temperature(const cJSON *v, char decimal) {
  if (!cJSON_IsNumber(v) && !cJSON_IsString(v))
    return nullptr;
  cJSON *ref = cJSON_CreateObject();
  if (cJSON_IsNumber(v)) {
    cJSON_AddStringToObject(ref, "text", number_text(v->valuedouble, decimal, 0).c_str());   // whole degrees, as an item's by default
    cJSON_AddStringToObject(ref, "unit", "°");
  } else {
    cJSON_AddStringToObject(ref, "text", v->valuestring);
  }
  return ref;
}
}  // namespace

/* Today as days since 1970 in local time; -1 while the panel has no time. */
long Engine::local_day() const {
  if (now_ < 1000000000)
    return -1;
  struct tm t;
  localtime_r(&now_, &t);
  return days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

/* The days of a weather feed ({"item", "codes"}): made from the item's state the first time they are asked for. */
const cJSON *Engine::weather_feed(const cJSON *ref) const {
  const std::string item = str(ref, "item");
  if (item.empty())
    return nullptr;
  const std::string codes = str(ref, "codes"), key = item + "|" + codes;
  auto it = weather_feeds_.find(key);
  if (it == weather_feeds_.end()) {
    WeatherFeed f;
    appoh::ItemState st;
    if (cb_.state && cb_.state(item, st) && !st.undefined()) {
      f.raw = std::move(st.state);
      f.have = true;
    }
    f.today = local_day();
    f.view = weather_feed_build(f, codes);
    it = weather_feeds_.emplace(key, std::move(f)).first;
  }
  return it->second.view;
}

/* {"days": [...], "now": {...}}: each day as a day of "days" has it, with fixed values -- {"day": days ahead, "title",
   "icon": {"code", "codes"}, "max", "min", "condition": {"text"}, "extras": [{"icon", "label", "color", "text"}]};
   none (no state yet, too long, no such JSON): one day that says so. */
cJSON *Engine::weather_feed_build(const WeatherFeed &f, const std::string &codes) const {
  cJSON *view = cJSON_CreateObject();
  cJSON *days = cJSON_AddArrayToObject(view, "days");
  const char decimal = settings_.decimal;
  std::string note;
  cJSON *doc = nullptr;
  if (!f.have || f.raw.empty())
    note = settings_.texts.undefined;
  else if (f.raw.size() > kMaxWeatherBytes)
    note = settings_.texts.feed_too_long;
  else if (!cJSON_IsArray(cJSON_GetObjectItem(doc = cJSON_ParseWithLength(f.raw.data(), f.raw.size()), "days")))
    note = f.raw;   // the rule's text (an error, say), or broken JSON
  if (note.empty()) {
    const std::string set = str(doc, "codes", codes.c_str());
    int i = -1, n = 0;   // i: the days of the list so far (what is no day does not count)
    for (const cJSON *in = cJSON_GetObjectItem(doc, "days")->child; in && n < kMaxWeatherDays; in = in->next) {
      if (!cJSON_IsObject(in))
        continue;
      i++;
      const long date = date_day(cJSON_GetStringValue(cJSON_GetObjectItem(in, "date")));
      const long ahead = date >= 0 && f.today >= 0 ? date - f.today : i;   // without a date: the first is today
      if (ahead < 0 || ahead > 15)   // gone (a list from yesterday), or beyond a forecast's reach
        continue;
      cJSON *d = cJSON_CreateObject();
      cJSON_AddNumberToObject(d, "day", static_cast<double>(ahead));
      if (const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(in, "title")))
        cJSON_AddStringToObject(d, "title", t);
      if (const std::string code = value_text(cJSON_GetObjectItem(in, "icon"), '.'); !code.empty()) {
        cJSON *icon = cJSON_AddObjectToObject(d, "icon");
        cJSON_AddStringToObject(icon, "code", code.c_str());
        if (const std::string c = str(in, "codes", set.c_str()); !c.empty())   // none: the default set
          cJSON_AddStringToObject(icon, "codes", c.c_str());
      }
      for (const char *key : {"max", "min"})
        if (cJSON *t = temperature(cJSON_GetObjectItem(in, key), decimal))
          cJSON_AddItemToObject(d, key, t);
      if (const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(in, "condition")); c && *c)
        cJSON_AddStringToObject(cJSON_AddObjectToObject(d, "condition"), "text", c);
      const cJSON *extras = cJSON_GetObjectItem(in, "extras");
      cJSON *out = nullptr;
      int k = 0;
      for (const cJSON *e = cJSON_IsArray(extras) ? extras->child : nullptr; e && k < kMaxWeatherExtras; e = e->next) {
        if (!cJSON_IsObject(e))
          continue;
        if (!out)
          out = cJSON_AddArrayToObject(d, "extras");
        cJSON *x = cJSON_CreateObject();
        for (const char *key : {"icon", "label", "color"})
          if (const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(e, key)))
            cJSON_AddStringToObject(x, key, v);
        cJSON_AddStringToObject(x, "text", value_text(cJSON_GetObjectItem(e, "value"), decimal).c_str());
        cJSON_AddItemToArray(out, x);
        k++;
      }
      cJSON_AddItemToArray(days, d);
      n++;
    }
    // the temperature now (home's outdoor temperature, when it has no item of its own)
    if (cJSON *t = temperature(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "now"), "temp"), decimal))
      cJSON_AddItemToObject(view, "now", t);
    if (!cJSON_GetArraySize(days))   // none from today on (an old list, an empty one): said so as for no state
      note = settings_.texts.undefined;
  }
  cJSON_Delete(doc);
  if (!note.empty()) {   // a day that says why there are none
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "day", 0);
    cJSON_AddBoolToObject(d, "note", true);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(d, "condition"), "text", note.c_str());
    cJSON_AddItemToArray(days, d);
  }
  return view;
}

/* Whether the page shown draws from the weather feed of `item` (home's, or the weather page's). */
bool Engine::shows_weather_feed(const std::string &item) const {
  if (current_ == "home")
    return shown_ == "home" && str(member(member(config_, "home"), "weather_feed"), "item") == item;
  const cJSON *page = page_config(current_);
  return type_of(page) == PageType::Weather && shown_ == "weather" && str(member(page, "feed"), "item") == item;
}

/* The item of a weather feed changed (or a day began: `day_begun`, every feed): its days made anew, and what shows them
   drawn again over itself -- home's weather block, the weather page's day --, each field said shown or hidden again.
   The days going are bound to nothing (a feed's values have no item): nothing holds them once drawn. */
void Engine::weather_feed_state(const std::string &item, bool day_begun) {
  std::vector<cJSON *> going;
  std::vector<std::string> changed;
  const long today = local_day();
  for (auto &[key, f] : weather_feeds_) {
    const std::string of = key.substr(0, key.find('|'));
    if (!day_begun && of != item)
      continue;
    appoh::ItemState st;
    const bool have = cb_.state && cb_.state(of, st) && !st.undefined();
    if (have == f.have && (!have || st.state == f.raw) && today == f.today)
      continue;
    f.have = have;
    f.raw = have ? std::move(st.state) : std::string();
    f.today = today;
    going.push_back(f.view);
    f.view = weather_feed_build(f, key.substr(key.find('|') + 1));
    changed.push_back(of);
  }
  for (const std::string &of : changed)
    if (shows_weather_feed(of)) {
      if (current_ == "home") {
        draw_home_weather(member(config_, "home"), false);
      } else {
        if (day_begun)   // the day shown stays the one shown: a sheet nearer now (draw_weather keeps it in the list)
          sheet_ = std::max(0, sheet_ - 1);
        draw_weather(page_config(current_));
        draw_dots();   // in the swipe row its sheets are dots of the row: as many as there are days now
      }
      break;
    }
  for (cJSON *v : going)
    cJSON_Delete(v);
}

/* A day begun (midnight, or the time came): the days before it go from the weather feeds. */
void Engine::weather_feed_tick() {
  const long today = local_day();
  if (today == weather_day_)
    return;
  weather_day_ = today;
  if (!weather_feeds_.empty())
    weather_feed_state("", true);
}

void Engine::weather_feeds_reset() {
  for (auto &[key, f] : weather_feeds_)
    cJSON_Delete(f.view);
  weather_feeds_.clear();
}

/* A value of the weather block on home: its own item ("weather_icon", "outdoor_high", "outdoor_low",
   "outdoor_temperature"), else today's from the weather feed ("weather_feed"). */
const cJSON *Engine::home_weather(const cJSON *home, const char *key) const {
  if (const cJSON *own = member(home, key))
    return own;
  const cJSON *view = weather_feed(member(home, "weather_feed"));
  if (!view)
    return nullptr;
  if (std::strcmp(key, "outdoor_temperature") == 0)
    return member(view, "now");
  const char *from = std::strcmp(key, "weather_icon") == 0 ? "icon" : std::strcmp(key, "outdoor_high") == 0 ? "max" : "min";
  for (const cJSON *d = member(view, "days") ? member(view, "days")->child : nullptr; d; d = d->next)
    if (!member(d, "note") && cJSON_IsNumber(member(d, "day")) && member(d, "day")->valueint == 0)
      return member(d, from);
  return nullptr;
}

}  // namespace appdisplay
