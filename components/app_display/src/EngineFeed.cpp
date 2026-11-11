/*
   An entity page's feed: its rows as JSON in one String item, which a rule in openHAB fills.

   As the page opens (or the screen wakes on it) the panel sends the item the command "refresh" and shows
   "Loading..." until a list comes; with "refresh" seconds it asks again while the page shows, and the list stays
   meanwhile. openHAB's autoupdate sets the item to "refresh" with the command, so the list that follows is always
   a change and reaches every panel -- one that is the same as the list drawn draws nothing anew. A rule may also
   set a list whenever it likes (without being asked). No list within "timeout" seconds: "No answer from openHAB"
   above the list the panel had.

   The item holds a list of rows, or {"rows": [...], "note": "..."}; anything else is a text the page shows (an
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

/* A row the panel draws from one of the list: its own keys only, the value as the row's fixed text (a number in
   the display language's decimal separator). */
cJSON *feed_row(const cJSON *in, const cJSON *page, char decimal) {
  if (cJSON_IsNull(in))
    return cJSON_CreateNull();
  if (!cJSON_IsObject(in))
    return nullptr;
  cJSON *row = cJSON_CreateObject();
  for (const char *key : {"label", "icon", "color", "value_color"})
    if (const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(in, key)))
      cJSON_AddStringToObject(row, key, s);
  const cJSON *v = cJSON_GetObjectItem(in, "value");
  if (const char *s = cJSON_GetStringValue(v)) {
    cJSON_AddStringToObject(row, "text", s);
  } else if (cJSON_IsNumber(v)) {
    char buf[40];
    if (std::fabs(v->valuedouble) < 1e15 && v->valuedouble == std::trunc(v->valuedouble))
      std::snprintf(buf, sizeof buf, "%.0f", v->valuedouble);
    else
      std::snprintf(buf, sizeof buf, "%.10g", v->valuedouble);
    if (char *dot = std::strchr(buf, '.'))
      *dot = decimal;
    cJSON_AddStringToObject(row, "text", buf);
  } else {
    cJSON_AddStringToObject(row, "text", "");
  }
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
    cJSON_AddItemToArray(rows, note_row(settings_.texts.feed_too_long, "alert-circle-outline", "yellow"));
    return;
  }
  // a list, or {"rows", "note"}; any other text is shown as it is
  const size_t start = feed_.raw.find_first_not_of(" \t\r\n");
  cJSON *doc = start != std::string::npos && (feed_.raw[start] == '[' || feed_.raw[start] == '{')
                   ? cJSON_ParseWithLength(feed_.raw.data(), feed_.raw.size())
                   : nullptr;
  const cJSON *list = cJSON_IsArray(doc) ? doc : cJSON_GetObjectItem(doc, "rows");
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

}  // namespace appdisplay
