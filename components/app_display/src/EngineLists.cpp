/*
   The button pages and the entity pages (Engine::draw_buttons, draw_entities).
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Cover.h"
#include "appdisplay/Icons.h"
#include "EngineDetail.h"

#include <qrcodegen.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace appdisplay {
using namespace detail;

namespace {
/* A button's icon in the middle of its tile, large: a matching rule's "layout", else its own "center"; every other
   button has it top left, the name at the bottom (the same on every tile, with a state or without). */
bool centred(const cJSON *b, const RuleStyle &style) {
  if (!style.layout.empty())
    return style.layout == "center";
  return str(b, "layout") == "center";
}

/* The parts of a button's tile that show: the icon top left, the name and its state at the bottom left -- or,
   centred, the icon in the middle large, the name centred under it (boards/nspanel/nextion/ohpanel_hmi.py
   buttons_page; the bar of a value in % is drawn, Engine::draw_button). */
std::vector<std::string> button_parts(bool centred) {
  if (centred)
    return {"_hs", "_pic", "_center", "_text"};
  return {"_hs", "_pic", "_icon", "_name", "_value"};
}
constexpr int kStateOn = 27469, kStateOff = 40147;   // a button's state line: #6b6b6b on the white tile, #9a9a9a on the dark
constexpr int kBarTrackOn = 57051, kBarTrackOff = 19017;   // its bar's track: #d9d9d9 on the white tile, #4a4a4a on the dark
}  // namespace

/* What tile i of a button page holds: its own button, or with a group the
   members of the sheet shown. */
const cJSON *Engine::button_at(const cJSON *page, int i) {
  if (!member(page, "group"))
    return slot(page, "buttons", i);
  const cJSON *row = cJSON_GetArrayItem(rows_of(page), sheet_ * rows_per_sheet(page) + i);
  return cJSON_IsObject(row) ? row : nullptr;
}

void Engine::draw_button(const cJSON *page, int i) {
  const cJSON *b = button_at(page, i);
  const std::string n = "tile" + std::to_string(i + 1);
  if (!b) {
    for (const char *part : {"_hs", "_pic", "_icon", "_name", "_value", "_bar", "_text", "_center"})
      vis(n + part, false);
    bar_[i] = false;
    return;
  }
  // active: always, else as its rules say (value_style, "active"), else by its state (on); its icon's place likewise
  const RuleStyle style = style_of(b);
  const bool center = centred(b, style);
  // the other layout's fields hidden (arrays, not initializer lists in a ?:, whose arrays end with the expression)
  static const std::vector<const char *> kCornerParts = {"_icon", "_name", "_value"}, kCenterParts = {"_center", "_text"};
  for (const char *part : center ? kCornerParts : kCenterParts)
    vis(n + part, false);
  const std::string action = str(b, "action", member(b, "open") ? "open" : member(b, "item") ? "toggle" : "none");
  // the small line: its "value", else by the item -- a percentage (a dimmer, a cover) as such, a switch (or a
  // contact) on / off -- whatever the button does when tapped; without a state yet as its action suggests
  const std::string type = has_state(b) ? state_of(b).type : "";
  const std::string value = str(b, "value", type == "Percent"                             ? "percent"
                                            : type == "OnOff" || type == "OpenClosed"     ? "on_off"
                                            : type.empty() && action == "toggle"          ? "on_off"
                                                                                          : "none");
  const bool active = flag(b, "always_active") || (style.active >= 0 ? style.active == 1 : on(b));
  const int bg = active ? kWhite : kDarkGrey;
  const int fg = active ? kDarkGrey : kLightGrey;
  // the tile in this slot's variant for the background
  const int tile = picture("button" + std::to_string(i + 1), active ? "on" : "off");
  if (tile >= 0)
    num(n + "_pic", "pic", tile);
  const std::string icon = n + (center ? "_center" : "_icon");
  const std::string name = n + (center ? "_text" : "_name");
  for (const std::string &part : {icon, name, n + "_value"})
    num(part, "bco", bg);
  // the icon: a rule's colour, else the accent (or the button's colour) while active; a rule's icon, blinking;
  // the state softer than the name -- a rule's colour colours it too, and the bar
  num(icon, "pco", style.color >= 0 ? style.color : active ? color_of(b, "color", theme_.accent) : theme_.dim);
  num(name, "pco", fg);
  num(n + "_value", "pco", style.color >= 0 ? style.color : active ? kStateOn : kStateOff);
  show_text(icon, style.icon.empty() ? glyph(b) : icon_glyph(style.icon.c_str()), style.blink);
  std::string v;
  int percent = -1;   // the bar's: a value in % shown as such
  if (value == "on_off") {
    v = active ? settings_.texts.on : settings_.texts.off;
  } else if (value == "percent") {   // a number, else (NULL, UNDEF, not known yet) "-"
    const bool known = has_state(b) && state_of(b).has_numeric && !state_of(b).undefined();
    if (known)
      percent = std::clamp(static_cast<int>(std::lround(state_of(b).numeric)), 0, 100);
    v = known ? std::to_string(percent) + " %" : settings_.texts.undefined;
  }
  txt(n + "_value", v);
  txt(name, str(b, "label"));
  // the bar (a progress bar in the design, hidden until shown): a track in the tile's tone, filled as far as the
  // value in the accent, or a rule's colour; none without a value in %
  bar_[i] = !center && percent >= 0;
  if (bar_[i]) {
    num(n + "_bar", "val", percent);
    num(n + "_bar", "pco", style.color >= 0 ? style.color : theme_.accent);
    num(n + "_bar", "bco", active ? kBarTrackOn : kBarTrackOff);
  } else {
    vis(n + "_bar", false);
  }
  // shown last: an object made visible draws at once, with what it has by then
  // (while the page is built, draw_buttons shows all buttons together)
  if (!building_buttons_) {
    for (const std::string &part : button_parts(center))
      vis(n + part, true);
    if (bar_[i])
      vis(n + "_bar", true);
  }
}

/* The page hides all its objects while it loads (vis 255,0). Showing them all
   again at once flashed every tile with the design's black default picture.
   So the title and the fixed parts come first, then the buttons are drawn
   hidden and shown together: with refreshing paused (ref_stop), the display
   draws them all in one go on ref_star. */
void Engine::draw_buttons(const cJSON *page) {
  txt("title", str(page, "title"));
  for (const char *obj : {"title", "close", "dots", "swipe"})
    vis(obj, true);
  if (member(page, "group")) {
    ask_group(page);
    if (const int sheets = sheets_of(page); !in_swipe_row())   // in the swipe row its dots instead (draw_dots)
      txt("dots", sheets > 1 ? sheet_dots(sheets, sheet_) : "");
  }
  building_buttons_ = true;
  for (int i = 0; i < 8; i++) {
    const cJSON *b = button_at(page, i);
    if (b && member(b, "item"))
      bind(b, [this, page, i] {
        // a state changed: the tile's picture and its fields' colours in one go -- drawn one by one, the fields
        // showed a moment in the old colour on the new tile
        if (building_buttons_)
          return draw_button(page, i);
        batch_refresh([this, page, i] { draw_button(page, i); });
      });
    else
      draw_button(page, i);
  }
  building_buttons_ = false;
  send("ref_stop");
  for (int i = 0; i < 8; i++)
    if (const cJSON *b = button_at(page, i)) {
      const std::string n = "tile" + std::to_string(i + 1);
      for (const std::string &part : button_parts(centred(b, style_of(b))))
        vis(n + part, true);
      if (bar_[i])
        vis(n + "_bar", true);
    }
  send("ref_star");
}

/* An entity page's style ("style": list, cards, rows): the cards hold eight a
   sheet, the list and the rows seven. A group's button page: its eight tiles. */
int Engine::rows_per_sheet(const cJSON *page) const {
  return type_of(page) == PageType::Buttons || str(page, "style") == "cards" ? 8 : 7;
}

/* An entity page's rows: its own, or its group's members (Callbacks::members)
   as rows with the page's icon, colour, rules and format -- made once they
   are known, kept until they change (on_options) or the configuration does.
   Null while not known. A group's button page has them as its tiles: those a
   tap can switch (Switch, Dimmer, Color), with "only_on" those that are on, or
   whose state is not known yet.

   With "metadata" (a namespace, e.g. "plant") each member's metadata there
   goes with its row ("meta": the value as "value", the config's keys; the
   rules' "@key"s), "label" a template of them ("{room} > {short_name}"),
   "hidden": true leaves the member out (shown without it), "icon" is its own
   instead of the page's, and "order" sorts -- those without after the others,
   by label as openHAB's list has them. */
const cJSON *Engine::rows_of(const cJSON *page) {
  if (member(page, "feed"))
    return feed_rows(page);   // the list an item holds (EngineFeed.cpp)
  const std::string group = str(page, "group");
  if (group.empty())
    return member(page, "rows");
  const std::string id = str(page, "id");
  if (const auto it = group_rows_.find(id); it != group_rows_.end())
    return it->second;
  std::vector<appoh::Member> members;
  if (!cb_.members || !cb_.members(group, members, false))
    return nullptr;
  const std::string ns = str(page, "metadata"), label = str(page, "label");
  static const appoh::Metadata kNone;
  const auto meta_of = [&](const appoh::Member &m) -> const appoh::Metadata & {
    const auto it = m.metadata.find(ns);
    return ns.empty() || it == m.metadata.end() ? kNone : it->second;
  };
  const auto order_of = [&](const appoh::Member &m) {
    const auto it = meta_of(m).config.find("order");
    char *end = nullptr;
    const double v = it == meta_of(m).config.end() ? 0 : std::strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() ? v : HUGE_VAL;
  };
  std::vector<const appoh::Member *> shown;
  const bool buttons = type_of(page) == PageType::Buttons;
  for (const appoh::Member &m : members) {
    if (buttons && !m.type.empty() && m.type != "Switch" && m.type != "Dimmer" && m.type != "Color")
      continue;
    const auto hidden = meta_of(m).config.find("hidden");
    if (hidden == meta_of(m).config.end() || hidden->second != "true")
      shown.push_back(&m);
  }
  std::stable_sort(shown.begin(), shown.end(),
                   [&](const appoh::Member *a, const appoh::Member *b) { return order_of(*a) < order_of(*b); });
  cJSON *rows = cJSON_CreateArray();
  for (const appoh::Member *mp : shown) {
    const appoh::Member &m = *mp;
    const appoh::Metadata &md = meta_of(m);
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "item", m.name.c_str());
    if (flag(page, "only_on") && has_state(row) && !on(row)) {
      cJSON_Delete(row);
      continue;
    }
    // without metadata there: the item's label
    std::string text = &md == &kNone ? m.label : label.empty() ? (md.value.empty() ? m.label : md.value) : label;
    if (&md != &kNone && !label.empty()) {   // {value}, {key}: from the metadata; {label}: the item's
      for (size_t at = text.find('{'); at != std::string::npos; at = text.find('{', at)) {
        const size_t close = text.find('}', at);
        if (close == std::string::npos)
          break;
        const std::string key = text.substr(at + 1, close - at - 1);
        const auto c = md.config.find(key);
        const std::string with = key == "value" ? md.value : key == "label" ? m.label : c != md.config.end() ? c->second : "";
        text.replace(at, close - at + 1, with);
        at += with.size();
      }
    }
    cJSON_AddStringToObject(row, "label", text.c_str());
    if (!ns.empty()) {
      cJSON *meta = cJSON_AddObjectToObject(row, "meta");
      cJSON_AddStringToObject(meta, "value", md.value.c_str());
      for (const auto &[k, v] : md.config)
        cJSON_AddStringToObject(meta, k.c_str(), v.c_str());
    }
    for (const char *key : {"icon", "color", "value_color", "value_style", "format", "factor"})
      if (const cJSON *v = member(page, key))
        cJSON_AddItemToObject(row, key, cJSON_Duplicate(v, true));
    if (const auto own = md.config.find("icon"); own != md.config.end()) {
      cJSON_DeleteItemFromObject(row, "icon");
      cJSON_AddStringToObject(row, "icon", own->second.c_str());
    }
    if (const auto badge = md.config.find("badge"); badge != md.config.end() && !badge->second.empty())
      cJSON_AddStringToObject(row, "badge", badge->second.c_str());   // a line badge in the icon's place
    cJSON_AddItemToArray(rows, row);
  }
  group_rows_[id] = rows;
  return rows;
}

/* What openHAB has as members now, once as the page opens, in the background (on_options when it differs). */
void Engine::ask_group(const cJSON *page) {
  if (const std::string group = str(page, "group"); !group.empty() && !group_asked_ && cb_.members) {
    group_asked_ = true;
    std::vector<appoh::Member> ignored;
    cb_.members(group, ignored, true);
  }
}

/* A list page's rows changed (a group's members, a feed's list): the page shown drawn again, on the sheet it
   was on as far as there still is one. */
void Engine::redraw_rows(const cJSON *page) {
  const cJSON *rows = rows_of(page);
  const int count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
  sheet_ = std::min(sheet_, std::max(0, (count - 1) / rows_per_sheet(page)));
  bindings_.clear();
  if (type_of(page) == PageType::Buttons)
    draw_buttons(page);
  else
    draw_entities(page, shown_);
  draw_dots();   // in the swipe row its sheets are dots of the row
}

void Engine::forget_group_rows() {
  for (auto &[id, rows] : group_rows_)
    cJSON_Delete(rows);
  group_rows_.clear();
}

namespace {
constexpr int kMaxBadgeChars = 3;
constexpr int kLabelAgeS = 60;   // a row's openHAB label asked for again as its page opens, when cached longer

/* A row's line badge ("badge": a bus line's "S12"): its pieces in the icon fonts (boards/nspanel/nextion/own_icons.py
   BADGE) side by side -- the start, its letters and digits (at most three, others left out), the end --, one badge
   in the colour the icon field draws it in, which the field centres as it does an icon (test/host/badge_test.py
   measures it); "" without one. */
std::string badge_of(const cJSON *row) {
  const std::string text = str(row, "badge");
  std::string pieces;
  int chars = 0;
  for (const char c : text) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (chars < kMaxBadgeChars && ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9'))) {
      pieces += icon_glyph((std::string("ohpanel-badge-") + lower).c_str());
      chars++;
    }
  }
  if (!chars)
    return {};
  return icon_glyph("ohpanel-badge-start") + pieces + icon_glyph("ohpanel-badge-end");
}
}  // namespace

/* What a row of an entity page is called: its own "label", else its item's label in openHAB as known
   (Callbacks::label), else -- not known yet, or none -- the item's name. */
std::string Engine::row_label(const cJSON *row) const {
  if (const cJSON *own = member(row, "label"); cJSON_IsString(own))
    return own->valuestring;
  const std::string item = str(row, "item");
  std::string label;
  if (!item.empty() && cb_.label && cb_.label(item, label, -1) && !label.empty())
    return label;
  return item;
}

/* The fixed rows of an entity page that show their item's label in openHAB: no label of their own, not wide (a
   wide row shows none). */
std::vector<const cJSON *> Engine::unlabelled_rows(const cJSON *page) const {
  std::vector<const cJSON *> out;
  if (type_of(page) != PageType::Entities || member(page, "group") || member(page, "feed") || flag(page, "wide"))
    return out;
  const cJSON *rows = member(page, "rows");
  for (const cJSON *r = cJSON_IsArray(rows) ? rows->child : nullptr; r; r = r->next)
    if (cJSON_IsObject(r) && !str(r, "item").empty() && !cJSON_IsString(member(r, "label")) && !flag(r, "wide"))
      out.push_back(r);
  return out;
}

/* The entity page in its style (boards/nspanel/nextion/ohpanel_hmi.py entities_pages()):
   a row is an icon with the label and the value, or with one wide field (the
   cards: the value without a label); the list draws a thin line between rows,
   the cards and the rows lie on their picture. Everything starts hidden: what
   a row has is shown.

   In the list and the rows a value that fits the narrow field (a percentage,
   watts) leaves the label more room: the long label and the narrow value
   show instead of the pair with the wide value column -- the display moves
   no objects, so a row has both and the value chooses, each time it changes. */
void Engine::draw_entities(const cJSON *page, const std::string &physical) {
  const bool cards = physical == "entities_cards", list = physical == "entities";
  const int per = rows_per_sheet(page);
  ask_group(page);
  if (member(page, "feed"))
    ask_feed(page);   // as the page opens: "Loading..." until the list comes
  const cJSON *all = rows_of(page);
  txt("title", title_of(page));   // a feed's own title, once its rows are built
  // fixed rows without a label of their own: their items' labels in openHAB, asked for again as the page opens
  // (all its rows, not only the sheet shown) when older than a minute
  if (!labels_asked_) {
    labels_asked_ = true;
    std::string label;
    for (const cJSON *r : unlabelled_rows(page))
      if (cb_.label)
        cb_.label(str(r, "item"), label, kLabelAgeS);
  }
  const int sheets = sheets_of(page);
  // the sheets as dots at the bottom, the one shown full -- in the swipe row its dots instead (draw_dots), not these
  // first
  if (!in_swipe_row()) {
    txt("dots", sheets > 1 ? sheet_dots(sheets, sheet_) : "");
    vis("dots", sheets > 1);
  }
  for (int r = 0; r < per; r++) {
    const cJSON *at = cJSON_GetArrayItem(all, sheet_ * per + r);
    const cJSON *row = cJSON_IsObject(at) ? at : nullptr;
    const bool wide = flag(page, "wide") || flag(row, "wide");   // the page's layout, or this row's
    const std::string n = (cards ? "card" : "row") + std::to_string(r + 1);
    const std::string obj = wide && !cards ? n + "_wide" : n + "_value";
    // the label and value fields of a row in the list and the rows (boards/nspanel/nextion/ohpanel_hmi.py
    // entities_pages): the pair with the wide value column or the one with the narrow, the wide field -- and the
    // scrolling label over them
    static const std::vector<const char *> kFields = {"_text", "_longtext", "_textscr", "_value", "_shortval", "_wide"};
    if (!row) {
      static const std::vector<const char *> kCard = {"_pic", "_icon", "_text", "_value"};
      if (cards) {
        for (const char *part : kCard)
          vis(n + part, false);
      } else {
        for (const char *part : kFields)
          vis(n + part, false);
        vis(n + "_icon", false);
        vis(n + (list ? "_line" : "_pic"), false);
      }
      continue;
    }
    if (!list)
      if (const int pic = picture(cards ? n : "rowcard" + std::to_string(r + 1), "box"); pic >= 0)
        num(n + "_pic", "pic", pic);
    const std::string label = wide ? "" : row_label(row);
    if (!list)
      vis(n + "_pic", true);
    vis(n + "_icon", true);
    if (cards) {
      txt(n + "_text", label);
      vis(n + "_text", !wide);
      vis(obj, true);
    }
    if (list)   // between this row and the next
      vis(n + "_line", r + 1 < per && cJSON_IsObject(cJSON_GetArrayItem(all, sheet_ * per + r + 1)));
    // a label too long for its field scrolls -- where the row says so, else the page (not the cards)
    const bool scroll = !cards && (member(row, "scroll") ? flag(row, "scroll") : flag(page, "scroll"));
    bind(row, [this, row, page, n, obj, cards, wide, scroll, label, physical] {
      const RuleStyle style = style_of(row, member(page, "value_style"));
      const std::string v = value_of(row, page, &style);
      std::string field = obj, lab;
      if (!cards) {
        // the fields the value and the label fit -- the wide value column leaves the label less room
        if (wide) {
          field = n + "_wide";
        } else {
          const bool narrow = fits(physical, n + "_shortval", v);
          field = n + (narrow ? "_shortval" : "_value");
          const std::string base = n + (narrow ? "_longtext" : "_text");
          lab = scroll && !fits(physical, base, label) ? n + "_textscr" : base;   // one scrolling label for both
        }
        // the ones going hidden first (they would paint over the others), a scrolling one standing still
        for (const char *part : kFields) {
          const std::string o = n + part;
          if (o == field || o == lab)
            continue;
          vis(o, false);
          if (o.size() > 3 && o.compare(o.size() - 3, 3, "scr") == 0)
            num(o, "en", 0);
        }
        if (!lab.empty()) {
          const bool rolls = lab.compare(lab.size() - 3, 3, "scr") == 0;
          txt(lab, rolls ? label + "     " : label);   // a gap before it comes round again
          vis(lab, true);
          if (rolls)
            num(lab, "en", 1);
        }
        vis(field, true);
      }
      // a rule may show another icon or colour it (icon_color), colour the value and let it blink
      // a rule's icon, else the row's line badge, else its icon
      const std::string badge = badge_of(row);
      txt(n + "_icon", !style.icon.empty() ? icon_glyph(style.icon.c_str()) : !badge.empty() ? badge : glyph(row));
      num(n + "_icon", "pco", style.icon_color >= 0 ? style.icon_color : color_of(row, "color", theme_.accent));
      show_text(field, v, style.blink);
      num(field, "pco", style.color >= 0 ? style.color : color_of(row, "value_color", theme_.text));
    });
  }
}



}  // namespace appdisplay
