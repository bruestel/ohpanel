/*
   The display logic.

   Object names are those of the display design (boards/nspanel/nextion/ohpanel_hmi.py); the
   mapping from the configuration to them follows the Arduino firmware's MainController.
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Cover.h"
#include "appdisplay/Icons.h"

#include <qrcodegen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "EngineDetail.h"

namespace appdisplay {
using namespace detail;


Engine::Engine(Sink &sink, Callbacks callbacks) : sink_(sink), cb_(std::move(callbacks)) {
  settings_ = display_settings(nullptr);
  settings_page_ = cJSON_Parse(R"({"id": "settings", "type": "settings"})");
  info_page_ = cJSON_Parse(R"({"id": "info", "type": "system"})");
}

Engine::~Engine() {
  cJSON_Delete(settings_page_);
  cJSON_Delete(info_page_);
  cJSON_Delete(member_page_);
  cJSON_Delete(config_);
  cJSON_Delete(weather_);
  forget_group_rows();
}

/* `text` as it fits the field: cut with "..." where it is wider (the display
   would cut it off at the edge). Widths from the design's font; a character
   outside the known range counts as "?". What counts is the ink: the blank
   columns of the first character before it and of the last after it may
   stick out (a 72 px "12:33" is 164 px of advances, its ink fits 160). */
std::string Engine::fitted(const std::string &page, const std::string &obj, const std::string &text) const {
  const auto p = fit_.find(page);
  if (p == fit_.end())
    return text;
  const auto o = p->second.find(obj);
  if (o == p->second.end())
    return text;
  const auto f = fonts_.find(o->second.font);
  if (f == fonts_.end() || f->second.widths.empty())
    return within_bytes(text, o->second.maxl);
  const FontWidths &fw = f->second;
  const auto index = [&fw](uint32_t c) { return fw.index(c); };
  const int room = o->second.width, dot = index('.');
  const int dots = 3 * fw.widths[dot] - fw.trail[dot];  // "..." up to its ink
  int used = 0, lead = 0;
  size_t cut = 0;  // the end of the last character that still leaves room for the dots
  for (size_t i = 0; i < text.size();) {
    const unsigned char b = text[i];
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    uint32_t c = n == 1 ? b : b & (0x7F >> n);
    for (size_t k = 1; k < n && i + k < text.size(); k++)
      c = c << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    const uint32_t k = index(c);
    if (i == 0)
      lead = fw.lead[k];
    used += fw.widths[k];
    if (used - fw.trail[k] - lead > room)
      return within_bytes(text.substr(0, cut) + "...", o->second.maxl);
    if (used + dots - lead <= room)
      cut = i + n;
    i += n;
  }
  return within_bytes(text, o->second.maxl);
}

/* No more than `maxl` bytes (a field's txt_maxl; 0: no limit): else cut at a character's start, "..." after it --
   the display would cut it itself, silently and in the middle of a letter's bytes ("Frühstüc"). */
std::string Engine::within_bytes(const std::string &text, int maxl) {
  if (maxl <= 0 || static_cast<int>(text.size()) <= maxl)
    return text;
  std::string body = text;
  if (body.size() >= 3 && body.compare(body.size() - 3, 3, "...") == 0)
    body.resize(body.size() - 3);
  size_t cut = static_cast<size_t>(std::max(0, maxl - 3));
  while (cut > 0 && (static_cast<unsigned char>(body[cut]) & 0xC0) == 0x80)
    cut--;   // not inside a character
  return body.substr(0, cut) + "...";
}

/* How wide `text` is in a font of the design; a font without known widths
   (the icon font) counts its height per character. */
bool Engine::fits(const std::string &page, const std::string &obj, const std::string &text) const {
  const auto p = fit_.find(page);
  const auto o = p == fit_.end() ? decltype(p->second.end()){} : p->second.find(obj);
  if (p == fit_.end() || o == p->second.end())
    return true;
  return text_width(o->second.font, text) <= o->second.width &&
         (o->second.maxl <= 0 || static_cast<int>(text.size()) <= o->second.maxl);
}

std::pair<std::string, std::string> Engine::two_lines(const std::string &page, const std::string &first,
                                                      const std::string &text) const {
  // the longest start that fits, ending at a space where there is one
  size_t cut = 0, space = std::string::npos;
  for (size_t i = 0; i < text.size();) {
    const unsigned char b = text[i];
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (!fits(page, first, text.substr(0, i + n)))
      break;
    i += n;
    cut = i;
    if (i < text.size() && text[i] == ' ')
      space = i;
  }
  if (cut >= text.size())
    return {text, ""};
  if (space != std::string::npos && space > 0)
    cut = space;
  std::string rest = text.substr(cut);
  while (!rest.empty() && rest[0] == ' ')
    rest.erase(0, 1);
  return {text.substr(0, cut), rest};
}

uint32_t Engine::FontWidths::index(uint32_t c) const {
  if (ranges.empty())
    return c >= first && c - first < widths.size() ? c - first : '?' - first;
  uint32_t at = 0, question = 0;
  for (const auto &[from, count] : ranges) {
    if (c >= from && c - from < count)
      return at + (c - from);
    if ('?' >= from && '?' - from < count)
      question = at + ('?' - from);
    at += count;
  }
  return question;
}

int Engine::text_width(int font, const std::string &text) const {
  const auto f = fonts_.find(font);
  int w = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char b = text[i];
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    uint32_t c = n == 1 ? b : b & (0x7F >> n);
    for (size_t k = 1; k < n && i + k < text.size(); k++)
      c = c << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    if (f == fonts_.end() || f->second.widths.empty()) {
      w += 24;
    } else {
      w += f->second.widths[f->second.index(c)];
    }
    i += n;
  }
  return w;
}

// ------------------------------------------------------------------ setup --

bool Engine::load_design(const char *manifest_json, const char *weather_json) {
  cJSON *m = cJSON_Parse(manifest_json);
  if (!m)
    return false;
  pages_.clear();
  backgrounds_.clear();
  const cJSON *bgs = member(m, "backgrounds");
  bg_themes_.clear();
  for (const cJSON *b = cJSON_IsArray(bgs) ? bgs->child : nullptr; b; b = b->next) {
    backgrounds_[str(b, "id")] = static_cast<int>(cJSON_GetNumberValue(member(b, "pic")));
    const cJSON *t = member(b, "theme");
    Theme theme;
    theme.accent = color_of(t, "accent", theme.accent);
    theme.text = color_of(t, "text", theme.text);
    theme.dim = color_of(t, "dim", theme.dim);
    bg_themes_[str(b, "id")] = theme;
  }
  default_background_ = str(m, "default_background");
  const cJSON *pages = member(m, "pages");
  for (const cJSON *p = cJSON_IsObject(pages) ? pages->child : nullptr; p; p = p->next) {
    PageInfo info;
    info.id = static_cast<int>(cJSON_GetNumberValue(member(p, "id")));
    info.background = flag(p, "background");
    info.close = flag(p, "close");
    for (const cJSON *c = member(p, "crop") ? member(p, "crop")->child : nullptr; c; c = c->next)
      if (cJSON_IsString(c))
        info.crop.emplace_back(c->valuestring);
    pages_[p->string] = info;
  }
  /* "pictures": background -> slot -> picture -> id (boards/nspanel/nextion/assets/compose.py) */
  pictures_.clear();
  const cJSON *pics = member(m, "pictures");
  for (const cJSON *bg = cJSON_IsObject(pics) ? pics->child : nullptr; bg; bg = bg->next)
    for (const cJSON *slot = bg->child; slot; slot = slot->next)
      for (const cJSON *pic = slot->child; pic; pic = pic->next)
        if (cJSON_IsNumber(pic))
          pictures_[bg->string][std::string(slot->string) + "/" + pic->string] = pic->valueint;
  fonts_.clear();
  const cJSON *fonts = member(m, "fonts");
  for (const cJSON *f = cJSON_IsObject(fonts) ? fonts->child : nullptr; f; f = f->next) {
    FontWidths fw;
    fw.first = static_cast<uint32_t>(cJSON_GetNumberValue(member(f, "first")));
    const auto list = [f](const char *key, std::vector<uint8_t> &out) {
      const cJSON *w = member(f, key);
      for (const cJSON *e = cJSON_IsArray(w) ? w->child : nullptr; e; e = e->next)
        out.push_back(static_cast<uint8_t>(e->valueint));
    };
    list("widths", fw.widths);
    list("lead", fw.lead);
    list("trail", fw.trail);
    size_t held = 0;
    const cJSON *runs = member(f, "ranges");
    for (const cJSON *r = cJSON_IsArray(runs) ? runs->child : nullptr; r; r = r->next) {
      const auto from = static_cast<uint32_t>(cJSON_GetNumberValue(cJSON_GetArrayItem(r, 0)));
      const auto count = static_cast<uint32_t>(cJSON_GetNumberValue(cJSON_GetArrayItem(r, 1)));
      fw.ranges.emplace_back(from, count);
      held += count;
    }
    if (held != fw.widths.size())   // not matching the lists: no run is trusted
      fw.ranges.clear();
    if (fw.lead.size() != fw.widths.size() || fw.trail.size() != fw.widths.size())
      fw.lead.assign(fw.widths.size(), 0), fw.trail.assign(fw.widths.size(), 0);
    fonts_[atoi(f->string)] = std::move(fw);
  }
  units_.clear();
  const cJSON *units = member(m, "units");
  for (const cJSON *u = cJSON_IsObject(units) ? units->child : nullptr; u; u = u->next) {
    UnitPlaces p;
    p.x = cJSON_GetNumberValue(member(u, "x"));
    p.step = std::max(1, static_cast<int>(cJSON_GetNumberValue(member(u, "step"))));
    p.n = cJSON_GetNumberValue(member(u, "n"));
    p.font = cJSON_GetNumberValue(member(u, "font"));
    p.field = str(u, "field");
    p.numbers = str(u, "numbers");
    units_[u->string] = p;
  }
  const cJSON *range = member(m, "range");
  const auto number = [range](const char *key, int dflt) {
    const cJSON *v = member(range, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
  };
  range_.step = number("step", 1);
  range_.n = number("n", 0);
  range_.font = number("font", 0);
  const cJSON *wl = member(m, "weather_line");
  const auto wnum = [wl](const char *key, int dflt) {
    const cJSON *v = member(wl, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
  };
  weather_line_ = {wnum("x0", 0), std::max(1, wnum("step", 1)), wnum("n", 0), wnum("font", 0), wnum("high_x", 0)};
  fit_.clear();
  const cJSON *fit = member(m, "fit");
  for (const cJSON *pg = cJSON_IsObject(fit) ? fit->child : nullptr; pg; pg = pg->next)
    for (const cJSON *o = pg->child; o; o = o->next)
      if (cJSON_GetArraySize(o) >= 2)   // [font, width, maxl]; the byte limit since design 2026-10-17
        fit_[pg->string][o->string] = {cJSON_GetArrayItem(o, 0)->valueint, cJSON_GetArrayItem(o, 1)->valueint,
                                       cJSON_GetArraySize(o) > 2 ? cJSON_GetArrayItem(o, 2)->valueint : 0};
  boxes_.clear();
  const cJSON *boxes = member(m, "boxes");
  for (const cJSON *pg = cJSON_IsObject(boxes) ? boxes->child : nullptr; pg; pg = pg->next)
    for (const cJSON *o = pg->child; o; o = o->next)
      if (cJSON_GetArraySize(o) == 4)
        boxes_[pg->string][o->string] = {cJSON_GetArrayItem(o, 0)->valueint, cJSON_GetArrayItem(o, 1)->valueint,
                                              cJSON_GetArrayItem(o, 2)->valueint, cJSON_GetArrayItem(o, 3)->valueint};
  cJSON_Delete(m);
  cJSON_Delete(weather_);
  weather_ = cJSON_Parse(weather_json);
  update_theme();
  return weather_ != nullptr && !pages_.empty();
}

void Engine::update_theme() {
  const auto it = bg_themes_.find(settings_.background.empty() ? default_background_ : settings_.background);
  theme_ = it != bg_themes_.end() ? it->second : Theme{};
  const cJSON *t = member(member(config_, "display"), "theme");
  theme_.accent = color_of(t, "accent", theme_.accent);
  theme_.text = color_of(t, "text", theme_.text);
  theme_.dim = color_of(t, "dim", theme_.dim);
  set_theme_colors(theme_.accent, theme_.text, theme_.dim);   // what "theme-accent" & co. stand for
  send_theme();
}

/* The theme in the display's global variables (th_accent, th_text, th_dim,
   boards/nspanel/nextion/ohpanel_hmi.py): the pages colour their fixed parts with them when
   they load, so those need no command each time. Sent again when the display
   restarts (its variables start at the design's defaults then). */
void Engine::send_theme() {
  /* The background as well (bg_pic, bg_close: boards/nspanel/nextion/ohpanel_hmi.py
     background_globals): the pages showing it take it before they draw.
     Before the start too: the boot page shows it, and its logo is the one of
     that background (draw_boot) -- with the design's default background
     under the logo of another one, it looked odd. */
  const std::string want = settings_.background.empty() ? default_background_ : settings_.background;
  const auto bg = backgrounds_.find(want) != backgrounds_.end() ? backgrounds_.find(want)
                                                                : backgrounds_.find(default_background_);
  const int pic = bg != backgrounds_.end() ? bg->second : -1, close = picture("close", "x");
  const std::string v = std::to_string(theme_.accent) + "," + std::to_string(theme_.text) + "," +
                        std::to_string(theme_.dim) + "," + std::to_string(pic) + "," + std::to_string(close);
  if (v == theme_sent_)
    return;
  const bool other_background = pic != bg_sent_;
  theme_sent_ = v;
  bg_sent_ = pic;
  send("th_accent=" + std::to_string(theme_.accent));
  send("th_text=" + std::to_string(theme_.text));
  send("th_dim=" + std::to_string(theme_.dim));
  if (pic >= 0)
    send("bg_pic=" + std::to_string(pic));
  if (close >= 0)
    send("bg_close=" + std::to_string(close));
  if (!started_ && other_background && shown_ == "boot")
    show_physical("boot");   // the boot page on it: loaded again (its fields take the background as they load)
}

void Engine::set_config(const std::string &json) {
  forget_group_rows();
  bindings_.clear();   // they point into the configuration going now: drawn again once the page is built anew
  cJSON_Delete(config_);
  config_ = cJSON_ParseWithLength(json.data(), json.size());
  dated_rules_ = json.find("\"within_days\"") != std::string::npos;
  const ScreenSettings before = settings_.screen;
  settings_ = display_settings(member(config_, "display"));
  screen_config(before);
  const std::string notify = str(member(member(config_, "display"), "notifications"), "item");
  if (notify != notify_item_) {
    notify_item_ = notify;
    notify_known_ = false;
  }
  update_theme();
  if (status_.openhab)
    prefetch_at_ = now_ms_ + kPrefetchDelayMs;   // a catalogue the new configuration names
  // the page shown may be gone
  if (current_ != "home" && !page_config(current_)) {
    current_ = "home";
    history_.clear();
  }
  if (started_)
    show_current();
}

void Engine::set_status(const PanelStatus &status) {
  const bool setup_changed = status.setup != status_.setup;
  if (status.openhab && !status_.openhab)
    prefetch_at_ = now_ms_ + kPrefetchDelayMs;   // the catalogues, once home has had its states
  status_ = status;
  if (started_ && setup_changed) {   // the setup page comes, or goes again
    show_current();
    return;
  }
  /* Not started (no network yet) and not knowing what the display shows --
     switched on with the panel, its boot page reported before anyone listened:
     the boot page asked for, so what it says gets drawn. */
  if (!started_ && shown_.empty() && expected_.empty()) {
    show_physical("boot");
    return;
  }
  if (shown_ == "boot")
    draw_boot();
  else if (shown_ == "home")
    draw_wifi();
}

/* The WiFi icon on home: only without a connection (red, crossed out). */
void Engine::draw_wifi() {
  constexpr int kRed = 63488;
  txt("wifi", status_.wifi ? "" : icon_glyph("wifi-off"));
  num("wifi", "pco", kRed);
}

void Engine::set_relay(int index, bool on) {
  if (index < 0 || index > 1)
    return;
  relays_[index] = on;
  if (shown_ == "home")
    draw_relays();
}

void Engine::start() {
  if (started_)
    return;
  started_ = true;
  current_ = "home";
  history_.clear();
  send_theme();
  if (shown_ == "boot" && boot_shown_ms_ >= 0 && now_ms_ - boot_shown_ms_ < kBootPageMinMs) {
    start_at_ms_ = boot_shown_ms_ + kBootPageMinMs;   // the boot page a moment longer (idle())
    return;
  }
  show_physical("home");
}

// ----------------------------------------------------------------- output --

/* While a notification is drawn nothing draws on the page under it; the
   display's variables and the page and brightness go through. */
void Engine::send(const std::string &command) {
  if (!link_)
    return;   // nobody to tell: drawn anew once the link is up (link_up)
  if (note_drawn_ && command.rfind("th_", 0) != 0 && command.rfind("bg_", 0) != 0 &&
      command.rfind("dim=", 0) != 0 && command.rfind("page ", 0) != 0)
    return;
  sink_.send(command);
}

void Engine::fill(int x, int y, int w, int h, int colour, bool over_note) {
  if (w <= 0 || h <= 0)
    return;
  const std::string c = "fill " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," +
                        std::to_string(h) + "," + std::to_string(colour);
  if (!over_note)
    send(c);
  else if (link_)
    sink_.send(c);
}

void Engine::disc(int x, int y, int r, int colour, bool over_note) {
  const std::string c = "cirs " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(r) + "," +
                        std::to_string(colour);
  if (!over_note)
    send(c);
  else if (link_)
    sink_.send(c);
}

/* Two bars and a disc in each corner. */
void Engine::rounded(int x, int y, int w, int h, int r, int colour, bool over_note) {
  fill(x + r, y, w - 2 * r, h, colour, over_note);
  fill(x, y + r, w, h - 2 * r, colour, over_note);
  for (const int cx : {x + r, x + w - 1 - r})
    for (const int cy : {y + r, y + h - 1 - r})
      disc(cx, cy, r, colour, over_note);
}

/* A pill: its ends discs of radius r (2r + 1 px across), the middle as high -- else the ends stand out --; the label
   drawn on it (a field's corners would stand out of it). */
void Engine::pill(const Box &b, int colour, int ink, const std::string &label) {
  const int r = (b.h - 1) / 2, top = b.y + (b.h - (2 * r + 1)) / 2;
  fill(b.x + r, top, b.w - 2 * r, 2 * r + 1, colour);
  disc(b.x + r, top + r, r, colour);
  disc(b.x + b.w - 1 - r, top + r, r, colour);
  send("xstr " + std::to_string(b.x + 6) + "," + std::to_string(top) + "," + std::to_string(b.w - 12) + "," +
       std::to_string(2 * r + 1) + ",2," + std::to_string(ink) + ",0,1,1,3,\"" + nextion::quote(label) + "\"");
}

void Engine::restore(int x, int y, int w, int h) {
  if (bg_sent_ >= 0 && w > 0 && h > 0)
    send("picq " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h) +
         "," + std::to_string(bg_sent_));
}

/* Values are only sent when they change: a Nextion object redraws on every
   assignment, even of the same text, which flickers. What a field shows
   before the first assignment since its page loaded is not known -- the
   design's text, or what was there when the page was not reloaded (the
   display outliving a restart of the panel) -- so that one is always sent,
   an empty text too. A text too wide for its field is shortened with "..."
   (the fields the manifest lists, of the page on screen), whoever writes it. */
void Engine::txt(const std::string &obj, const std::string &whole) {
  const std::string text = fitted(shown_, obj, whole);
  const std::string key = obj + ".txt";
  const auto it = sent_.find(key);
  if (it != sent_.end() && it->second == text)
    return;
  sent_[key] = text;
  send(obj + ".txt=\"" + nextion::quote(text) + "\"");
}

void Engine::num(const std::string &obj, const char *attr, int value) {
  const std::string v = std::to_string(value);
  auto &last = sent_[obj + "." + attr];
  if (last == v)
    return;
  last = v;
  send(obj + "." + attr + "=" + v);
}

/* Back to the colour the page gave obj when it loaded (a th_* variable), if
   obj got another one since: nothing to send otherwise. */
void Engine::page_colour(const std::string &obj, int value) {
  if (sent_.count(obj + ".pco"))
    num(obj, "pco", value);
}

void Engine::vis(const std::string &obj, bool visible) {
  const std::string v = visible ? "1" : "0";
  auto &last = sent_[obj + ".vis"];
  if (last == v)
    return;
  last = v;
  send("vis " + obj + "," + v);
}

// ------------------------------------------------------------------ model --

const cJSON *Engine::page_config(const std::string &id) const {
  for (const cJSON *p = member(config_, "pages") ? member(config_, "pages")->child : nullptr; p; p = p->next)
    if (str(p, "id") == id)
      return p;
  // the settings page is built in: always there, never configured; so is the
  // info page, which a configuration may give a title (a page "info")
  if (id == "settings")
    return settings_page_;
  if (id == "info")
    return info_page_;
  if (id == "@cover")   // a member of a cover group page (covers_event)
    return member_page_;
  return nullptr;
}

/* The design's pages are named after the page types that draw on them
   (boards/nspanel/nextion/ohpanel_hmi.py); an entity page has one per style. */
std::string Engine::template_of(const cJSON *page) const {
  const std::string type = str(page, "type");   // the design's page of the same name, but for these
  if (type_of(page) == PageType::MediaFavorites) return "buttons";   // tiles as on a button page
  if (type_of(page) == PageType::Cover && member(page, "group") && pages_.count("covers")) return "covers";   // its rows
  if (type_of(page) == PageType::Entities) {
    const std::string style = str(page, "style");
    if ((style == "cards" || style == "rows") && pages_.count("entities_" + style))
      return "entities_" + style;
  }
  return pages_.count(type) ? type : "home";
}

appoh::ItemState Engine::state_of(const cJSON *ref) const {
  appoh::ItemState s;
  const std::string item = str(ref, "item");
  if (item.empty() || !cb_.state || !cb_.state(item, s)) {
    s = appoh::ItemState{};
  }
  return s;
}

bool Engine::has_state(const cJSON *ref) const {
  appoh::ItemState s;
  const std::string item = str(ref, "item");
  return !item.empty() && cb_.state && cb_.state(item, s);
}

std::string Engine::value_of(const cJSON *ref, const cJSON *dflt, const RuleStyle *style) const {
  if (!ref)
    return {};
  if (member(ref, "text"))
    return str(ref, "text");
  if (!has_state(ref))
    return {};
  const auto from = [&](const char *key) { return member(ref, key) ? member(ref, key) : dflt ? member(dflt, key) : nullptr; };
  const cJSON *fmt = from("format"), *factor = from("factor"), *decimals = from("decimals");
  if (style && !style->format.empty())
    return format_state(state_of(ref), style->format.c_str(), settings_, style->factor);
  return format_state(state_of(ref), cJSON_IsString(fmt) ? fmt->valuestring : "", settings_,
                      cJSON_IsNumber(factor) ? factor->valuedouble : 1, cJSON_IsNumber(decimals) ? decimals->valueint : 0);
}

void Engine::number_and_unit(const cJSON *ref, std::string &number, std::string &unit,
                             const RuleStyle *style) const {
  unit.clear();
  const appoh::ItemState s = has_state(ref) ? state_of(ref) : appoh::ItemState{};
  const bool numeric = s.has_numeric && (s.type == "Quantity" || s.type == "Decimal" || s.type == "Percent");
  if (!numeric || member(ref, "format") || member(ref, "text") || (style && !style->format.empty())) {
    number = value_of(ref, nullptr, style);
    return;
  }
  const cJSON *d = member(ref, "decimals");
  const int decimals = cJSON_IsNumber(d) ? std::clamp(d->valueint, 0, 3) : 0;
  const cJSON *f = member(ref, "factor");
  const double factor = cJSON_IsNumber(f) ? f->valuedouble : 1;
  number = format_number(s.numeric * factor, decimals, false, settings_.decimal);
  // scaled, it is another unit: a format says which
  unit = factor == 1 ? unit_of(s) : "";
}

/* The unit right after the number: of the places one digit apart, the first
   past the number's width. */
/* A number and its unit on home (manifest "units", home_layout in
   boards/nspanel/nextion/ohpanel_hmi.py): with a unit, the number in the narrow field as wide
   as it (numbers 1..n, one digit apart) and the unit right after it (prefix
   1..n); without, the text in the wide field. A field repaints its whole box
   when its text changes, so the narrow field ends where its unit begins; and
   hiding a field repaints the background over its box, so all that go are
   hidden before the one that comes shows. */
void Engine::show_number(const std::string &prefix, bool shown, const std::string &number,
                         const std::string &unit, int colour, int blink) {
  const auto it = units_.find(prefix);
  if (it == units_.end())
    return;
  const UnitPlaces &p = it->second;
  int k = 0;  // 0: the wide field
  if (shown && !unit.empty()) {
    const int width = text_width(p.font, number);
    k = std::clamp((width + p.step - 1) / p.step, 1, std::max(1, p.n));
  }
  const auto place = [&](int j, bool visible) {
    if (j == 0) {
      vis(p.field, visible);
      if (!visible)
        anims_.erase(p.field);  // blinking stops with it
    } else {
      vis(p.numbers + std::to_string(j), visible);
      vis(prefix + std::to_string(j), visible);
      if (!visible) {
        anims_.erase(p.numbers + std::to_string(j));
        anims_.erase(prefix + std::to_string(j));
      }
    }
  };
  // a rule's colour, else the page's (the number th_text, its unit th_dim); blinking with it
  const auto put = [&](const std::string &obj, const std::string &text, int page) {
    if (colour >= 0)
      num(obj, "pco", colour);
    else
      page_colour(obj, page);
    show_text(obj, text, blink);
  };
  for (int j = 0; j <= p.n; j++)
    if (j != k || !shown)
      place(j, false);
  if (!shown)
    return;
  if (k == 0) {
    put(p.field, number, theme_.text);
  } else {
    put(p.numbers + std::to_string(k), number, theme_.text);
    put(prefix + std::to_string(k), unit, theme_.dim);
  }
  place(k, true);
}

/* Today's low and high under the outdoor temperature: "↓9° ↑22°"
   (home_page in boards/nspanel/nextion/ohpanel_hmi.py, manifest "range"). The low in the
   field as wide as it (low1..n, half digits), the high's arrow and value
   right after it (high_arrowN/highN, the same place; place 0, at the low's own
   place, for a high without a low). Fields that go are hidden first: hiding
   repaints the background over a field's box. */
void Engine::draw_range(const cJSON *home) {
  const cJSON *lo = member(home, "outdoor_low"), *hi = member(home, "outdoor_high");
  const auto text = [this](const cJSON *ref) {
    std::string number, unit;
    number_and_unit(ref, number, unit);
    return number + degrees_only(unit);
  };
  const std::string low = lo ? text(lo) : "", high = hi ? text(hi) : "";
  const int n = std::max(1, range_.n), step = std::max(1, range_.step);
  const int k = lo ? std::clamp((text_width(range_.font, low) + step - 1) / step, 1, n) : 0;
  const auto at = [](const char *part, int j) { return std::string(part) + std::to_string(j); };
  if (!lo)
    vis("low_arrow", false);
  for (int j = 0; j <= n; j++) {
    if (j && (!lo || j != k))
      vis(at("low", j), false);
    if (!hi || j != k) {
      vis(at("high_arrow", j), false);
      vis(at("high", j), false);
    }
  }
  if (lo) {
    txt(at("low", k), low);
    vis("low_arrow", true);
    vis(at("low", k), true);
  }
  if (hi) {
    txt(at("high", k), high);
    vis(at("high_arrow", k), true);
    vis(at("high", k), true);
  }
}

/* A temperature in the weather block as weather apps show it: "°" without
   the scale ("°C" -> "°"); any other unit as it is, after a space. */
std::string Engine::degrees_only(const std::string &unit) {
  if (unit.rfind("°", 0) == 0)
    return "°";
  return unit.empty() ? unit : " " + unit;
}

/* Whether ref's item is on: the states its "on" names (a state or a list,
   e.g. "PLAYING" of a player's String item), or else ON, OPEN, PLAY or a
   number above 0. */
bool Engine::on(const cJSON *ref) const {
  if (!has_state(ref))
    return false;
  const appoh::ItemState s = state_of(ref);
  const cJSON *want = member(ref, "on");
  if (cJSON_IsString(want))
    return s.state == want->valuestring;
  if (cJSON_IsArray(want)) {
    for (const cJSON *e = want->child; e; e = e->next)
      if (cJSON_IsString(e) && s.state == e->valuestring)
        return true;
    return false;
  }
  return is_on(s);
}

/* The command that flips an item: ON or OFF for the types that take them
   (Switch, Dimmer, Color), decided by the state the panel knows -- explicit,
   so it also works on an item without a binding, where openHAB has nothing
   to resolve a TOGGLE against. TOGGLE only when the state is unknown or the
   type takes neither. */
std::string Engine::toggle_command(const cJSON *ref) const {
  if (!has_state(ref))
    return "TOGGLE";
  const appoh::ItemState s = state_of(ref);
  if (s.type == "OnOff" || s.type == "Percent" || s.type == "HSB")
    return is_on(s) ? "OFF" : "ON";
  return "TOGGLE";
}

/* The picture for `name` in a slot, in the variant of the background in use
   (the display has no transparency, see boards/nspanel/nextion/assets/compose.py); -1 if the
   design has none. */
int Engine::picture(const std::string &slot, const std::string &name) const {
  auto bg = pictures_.find(settings_.background.empty() ? default_background_ : settings_.background);
  if (bg == pictures_.end())
    bg = pictures_.find(default_background_);
  if (bg == pictures_.end())
    return -1;
  const auto it = bg->second.find(slot + "/" + name);
  return it == bg->second.end() ? -1 : it->second;
}

/* The weather icon for the condition code in `ref`'s item, from the icon set
   it names (boards/nspanel/nextion/weather-icons.json: code -> icon name). */
std::string Engine::weather_icon(const cJSON *ref) const {
  const cJSON *set = member(weather_, str(ref, "codes", "openweathermap").c_str());
  if (!set)
    return "";
  std::string name = str(set, "default");
  if (has_state(ref)) {
    const cJSON *icon = member(member(set, "codes"), state_of(ref).state.c_str());
    if (cJSON_IsString(icon))
      name = icon->valuestring;
  }
  return name;
}

int Engine::weather_pic(const cJSON *ref, const char *slot) const {
  const std::string name = weather_icon(ref);
  return name.empty() ? -1 : picture(slot, name);
}

std::string Engine::glyph(const cJSON *obj, const char *key) const {
  const cJSON *v = member(obj, key);
  // a list of icons (an indicator's animation): its first
  return icon_glyph(cJSON_IsArray(v) ? cJSON_GetStringValue(cJSON_GetArrayItem(v, 0)) : str(obj, key).c_str());
}

/* An indicator's icon in obj: one, or a list of them shown in turn every
   "interval" ms (animate(); a playing speaker's waves, say). */
/* What the first matching rule of `ref`'s value_style (or `rules`) sets for
   its item's state; nothing without a state. */
RuleStyle Engine::style_of(const cJSON *ref, const cJSON *rules) const {
  const cJSON *list = member(ref, "value_style") ? member(ref, "value_style") : rules;
  if (!list || !has_state(ref))
    return {};
  return rule_style(list, state_of(ref), now_, member(ref, "meta"));   // a group member's metadata: its "@key"s
}

/* Frames shown in turn in obj, interval_ms each (one: just shown); one that
   goes on with the same frames goes on where it is. */
void Engine::show_frames(const std::string &obj, std::vector<std::string> frames, int interval_ms) {
  if (frames.size() < 2) {
    anims_.erase(obj);
    txt(obj, frames.empty() ? "" : frames[0]);
    return;
  }
  const auto old = anims_.find(obj);
  if (old != anims_.end() && old->second.frames == frames) {
    old->second.interval_ms = interval_ms;
    return;
  }
  Animation a;
  a.frames = std::move(frames);
  a.interval_ms = interval_ms;
  txt(obj, a.frames[0]);
  anims_[obj] = std::move(a);
}

/* A text, blinking (shown and gone, blink_ms each) when a rule says so. */
void Engine::show_text(const std::string &obj, const std::string &text, int blink_ms) {
  if (blink_ms > 0 && !text.empty())
    show_frames(obj, {text, ""}, std::clamp(blink_ms, 200, 2000));
  else
    show_frames(obj, {text}, 0);
}

/* An indicator's icon in obj, in `colour` -- one, or a list of them shown in
   turn every "interval" ms; blinking with "blink". A matching rule of its
   value_style may set another icon, colour and blinking. */
void Engine::show_icon(const std::string &obj, const cJSON *ref, int colour) {
  const RuleStyle style = style_of(ref);
  const cJSON *icon = member(ref, "icon");
  std::vector<std::string> frames;
  int interval = 500;
  if (!style.icon.empty()) {
    frames = {icon_glyph(style.icon.c_str())};
  } else if (cJSON_IsArray(icon) && cJSON_GetArraySize(icon) >= 2) {
    for (const cJSON *e = icon->child; e; e = e->next)
      frames.push_back(icon_glyph(cJSON_GetStringValue(e) ? e->valuestring : ""));
    const cJSON *iv = member(ref, "interval");
    interval = cJSON_IsNumber(iv) ? std::clamp(iv->valueint, 200, 5000) : 500;
  } else {
    frames = {glyph(ref)};
  }
  const cJSON *b = member(ref, "blink");
  const int blink = style.blink > 0 ? style.blink : cJSON_IsNumber(b) ? b->valueint : 0;
  if (frames.size() == 1 && blink > 0) {
    frames.push_back("");
    interval = std::clamp(blink, 200, 2000);
  }
  show_frames(obj, std::move(frames), interval);
  num(obj, "pco", style.color >= 0 ? style.color : colour);
}

void Engine::animate(int64_t now_ms) {
  if (shown_.empty())   // the display away: nothing (a command in an upload breaks it)
    return;
  for (auto &[obj, a] : anims_) {
    if (a.next_ms < 0 || now_ms - a.next_ms > 5000) {   // just started, or a long pause: from now
      a.next_ms = now_ms + a.interval_ms;
      continue;
    }
    if (now_ms < a.next_ms)
      continue;
    a.at = (a.at + 1) % a.frames.size();
    txt(obj, a.frames[a.at]);
    a.next_ms += a.interval_ms;
  }
}

/* Remembers what to redraw when the item of `ref` changes, and draws it now. */
void Engine::bind(const cJSON *ref, std::function<void()> draw) {
  const std::string item = str(ref, "item");
  if (!item.empty())
    bindings_.push_back({item, draw});
  draw();
}

// ------------------------------------------------------------- navigation --

void Engine::show_physical(const std::string &wanted) {
  const std::string name = status_.setup ? "boot" : wanted;  // the setup page stays while it is wanted
  const auto it = pages_.find(name);
  if (it == pages_.end())
    return;
  expected_ = name;
  // what was still to be drawn belongs to the page going: on the next it would land wherever it lands
  logo_queue_.clear();
  cover_job_.reset();
  send("page " + std::to_string(it->second.id));
  note_drawn_ = false;
}

void Engine::open(const std::string &id, bool remember) {
  const cJSON *page = id == "home" ? nullptr : page_config(id);
  if (id != "home" && !page)
    return;
  if (remember && id != current_)
    history_.push_back(current_);
  if (id == "home")
    history_.clear();
  current_ = id;
  sheet_ = 0;
  light_view_ = 0;
  station_sheet_ = 0;
  favorites_asked_ = false;
  group_asked_ = false;
  show_physical(page ? template_of(page) : "home");
}

void Engine::back() {
  std::string target = "home";
  while (!history_.empty()) {
    target = history_.back();
    history_.pop_back();
    if (target == "home" || page_config(target))
      break;
    target = "home";
  }
  open(target, false);
}

/* left/right walk the configured row -- or the sheets of a long entity list (not in the row). */
void Engine::swipe(const std::string &direction) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if ((shown_ == "cover" && now_ms_ - cover_drag_.since_ms < 1000) ||
      (shown_ == "thermostat" && now_ms_ - thermo_drag_.since_ms < 1000))
    return;   // a finger moving on the cover's drawing, on the thermostat's ring: not a swipe
  if (type_of(page) == PageType::Entities || type_of(page) == PageType::Weather ||
      (type_of(page) == PageType::Cover && member(page, "group"))) {
    // the finger moving left brings the next sheet (rows, a day), as the next page in the row
    if (direction != "left" && direction != "right")
      return;
    int sheets = static_cast<int>(days_of(page).size());
    if (type_of(page) == PageType::Cover)   // the group's rows, the whole group first: four a sheet
      sheets = std::max(1, (static_cast<int>(cover_rows(page).size()) + cover_rows_per_sheet(page) - 1) /
                               cover_rows_per_sheet(page));
    if (type_of(page) == PageType::Entities) {
      const cJSON *rows = rows_of(page);
      const int count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
      const int per = rows_per_sheet(page);
      sheets = std::max(1, (count + per - 1) / per);
    }
    const int next = sheet_ + (direction == "left" ? 1 : -1);
    if (next >= 0 && next < sheets) {
      sheet_ = next;
      show_physical(template_of(page));
    }
    return;
  }
  if (type_of(page) == PageType::MediaFavorites) {   // sheets of stations: the finger moving left or up brings the next
    const int count = static_cast<int>(stations_of(page).size());
    const int sheets = std::max(1, (count + 7) / 8);
    const int next = station_sheet_ + (direction == "left" || direction == "up" ? 1 : -1);
    if (next >= 0 && next < sheets) {
      station_sheet_ = next;
      show_physical(template_of(page));
    }
    return;
  }
  if (direction != "left" && direction != "right")
    return;
  const std::vector<std::string> row = swipe_row();
  const int n = static_cast<int>(row.size());
  const auto it = std::find(row.begin(), row.end(), current_);
  if (it == row.end() || n < 2)
    return;
  // the finger moving left brings the next page in, as on a phone
  const int at = static_cast<int>(it - row.begin());
  fade_to(row[(at + (direction == "left" ? 1 : n - 1)) % n]);
}

/* The swipe's page change, softened: the backlight down in a few steps, the
   page, and once it has loaded and been drawn, up again -- the display cannot
   move a page, and this hides its drawing line by line. The steps go out
   from idle(), one every fade_step_ms_; the brightness in use (level_) stays
   what screen_tick() keeps -- the steps are sent past it. */
namespace {
constexpr int kFadeSteps = 4;
constexpr int64_t kFadeWaitMs = 1500;   // the page not there by then: the light back anyway
}  // namespace

void Engine::fade_to(const std::string &id) {
  // display.page_fade: so long each way, in kFadeSteps steps
  fade_step_ms_ = settings_.page_fade / kFadeSteps;
  if (fade_step_ms_ <= 0 || level_ <= 0 || asleep_) {
    open(id, false);
    return;
  }
  fade_ = Fade{1, 0, now_ms_ - fade_step_ms_, id};   // the first step at once
  fade_tick();
}

void Engine::fade_tick() {
  if (!fade_.phase || now_ms_ - fade_.at < fade_step_ms_)
    return;
  const int full = std::max(level_, 0);
  if (fade_.phase == 1) {
    fade_.at = now_ms_;
    fade_.step++;
    send("dim=" + std::to_string(full * (kFadeSteps - fade_.step) / kFadeSteps));
    if (fade_.step >= kFadeSteps) {
      fade_.phase = 2;
      open(fade_.target, false);
    }
  } else if (fade_.phase == 2) {
    if (now_ms_ - fade_.at >= kFadeWaitMs) {   // never loaded: back to the brightness in use
      send("dim=" + std::to_string(full));
      fade_ = Fade{};
    }
  } else if (fade_.phase == 3) {
    fade_.at = now_ms_;
    fade_.step++;
    send("dim=" + std::to_string(full * fade_.step / kFadeSteps));
    if (fade_.step >= kFadeSteps)
      fade_ = Fade{};
  }
}

/* The pages reached by swiping, in order: home always first -- the start
   screen -- then the configuration's swipe row without it (an older one may
   name home elsewhere, or not at all). */
/* The swipe row: home first, then the configured button pages -- the only
   ones that may be in it (the configuration checks that too, Schema); the
   others open from a tile or a shortcut. */
std::vector<std::string> Engine::swipe_row() const {
  std::vector<std::string> row{"home"};
  const cJSON *list = member(config_, "swipe");
  for (const cJSON *e = cJSON_IsArray(list) ? list->child : nullptr; e; e = e->next) {
    if (!cJSON_IsString(e) || std::string(e->valuestring) == "home")
      continue;
    if (type_of(page_config(e->valuestring)) == PageType::Buttons)
      row.push_back(e->valuestring);
  }
  return row;
}

void Engine::on_page_loaded(const std::string &name) {
  shown_ = name;
  slider_moved_ms_ = -100000;   // a page anew: no finger on its slider
  slider_resync_ = false;
  slider_left_ = -1;
  cover_drag_.clear();
  thermo_sent_.clear();
  mode_tapped_.clear();
  button_lit_.clear();
  note_drawn_ = false;  // the page covers it
  sent_.clear();
  drawn_.clear();
  logo_queue_.clear();
  cover_job_.reset();
  bindings_.clear();
  anims_.clear();
  if (name == "boot") {
    // The boot page while another one is on its way (asked for since): gone
    // before anything drawn on it arrives -- it does not count as shown
    // (set_status would draw on it)
    if (expected_.empty() || expected_ == "boot") {
      expected_.clear();
      if (!started_)
        boot_shown_ms_ = now_ms_;
      draw_boot();
    } else {
      shown_.clear();
    }
    // a started panel goes to its page -- unless the boot page is what it wants (the setup hotspot opened
    // by hand): asked for again, it loaded again and again, flickering
    if (started_ && !status_.setup && expected_ != "boot")
      show_current();
    return;
  }
  if (name == "blank") {  // the black page of the night: nothing on it
    expected_.clear();
    return;
  }
  if (!started_ && name != expected_) {
    // The display moved by itself before the panel started (its own program
    // going home, say): back to the boot page -- home would show what is not
    // there yet, the crossed-out WiFi icon first.
    show_physical("boot");
    return;
  }
  if (name != expected_) {
    // The display moved by itself (to home, e.g. after its own restart).
    if (name == "home") {
      current_ = "home";
      history_.clear();
    } else {
      return;  // a page this firmware does not drive (settings)
    }
  }
  expected_.clear();
  render();
  if (fade_.phase == 2) {   // the swipe's page there: the light up again (fade_tick)
    fade_.phase = 3;
    fade_.step = 0;
    fade_.at = now_ms_;
  }
  draw_note();
}

/* The busy indicator's dots (boards/nspanel/nextion/ohpanel_hmi.py busy_indicator); sent as
   they are, past the cache: "vis 255,1" shows them too. */
void Engine::hide_busy() {
  for (const char *dot : {"busy1", "busy2", "busy3"})
    send(std::string("vis ") + dot + ",0");
}

/* The display shows nothing of ours (it restarts, or is being rewritten):
   nothing is drawn until it reports a page again -- the bindings would write to
   objects of a page it does not show. */
void Engine::display_away() {
  level_ = -1;  // a display that restarts comes back at its own brightness
  anims_.clear();
  shown_.clear();
  expected_.clear();
  sent_.clear();
  drawn_.clear();
  logo_queue_.clear();
  cover_job_.reset();
  bindings_.clear();
  theme_sent_.clear();
  bg_sent_ = -1;
}

void Engine::link_down() {
  link_ = false;
  display_away();
}

/* The display found (at the start, after it restarted or was rewritten):
   whatever it shows, the theme and the page it should show -- the boot page
   until the panel has started. */
void Engine::link_up() {
  display_away();
  link_ = true;
  send_theme();
  if (started_)
    show_current();
  else
    show_physical("boot");
}

void Engine::render() {
  if (shown_ == "volpage") {   // the media page's volume, a page of its own
    draw_volume_page();
    return;
  }
  if (shown_ == "netpage") {   // the info page's network details, over it
    draw_network_page();
    return;
  }
  // the page's busy indicator (boards/nspanel/nextion/ohpanel_hmi.py busy_indicator) stays away
  send("busy_tm.en=0");
  hide_busy();
  if (current_ == "home") {
    // built with the display's refreshing paused, drawn in one go
    send("ref_stop");
    refresh_paused_ = true;
    draw_home();
    refresh_paused_ = false;
    send("ref_star");
  } else if (const cJSON *page = page_config(current_)) {
    switch (type_of(page)) {
      case PageType::Buttons: draw_buttons(page); break;
      case PageType::MediaFavorites: draw_favorites(page); break;
      case PageType::Entities: draw_entities(page, shown_); break;
      case PageType::Cover: draw_cover(page); break;
      case PageType::Thermostat: draw_thermostat(page); break;
      case PageType::Light: draw_light(page); break;
      case PageType::Media: draw_media(page); break;
      case PageType::Weather: draw_weather(page); break;
      case PageType::Qr: draw_qr(page); break;
      case PageType::System: draw_system(page); break;
      case PageType::Settings: draw_settings(page); break;
      case PageType::None: break;
    }
  }
  draw_dots();
}

/* The design ships with one background; another one is set at runtime on the
   page and on every object that crops from it, then the page is redrawn. */

void Engine::prefetch_options() {
  std::vector<appoh::Option> out;
  appoh::Limits l;
  const cJSON *pages = member(config_, "pages");
  for (const cJSON *pg = cJSON_IsArray(pages) ? pages->child : nullptr; pg; pg = pg->next) {
    const PageType type = type_of(pg);
    if (type == PageType::MediaFavorites && cb_.options) {
      if (const std::string item = str(member(pg, "stations"), "item"); !item.empty())
        cb_.options(item, out, true);
    } else if (type == PageType::Light && cb_.limits && (!member(pg, "min") || !member(pg, "max"))) {
      if (const std::string item = str(member(pg, "color_temperature"), "item"); !item.empty())
        cb_.limits(item, l);
    } else if (type == PageType::Thermostat) {   // the target's range and step, the modes: there when it opens
      if (cb_.limits && !str(pg, "item").empty())
        cb_.limits(str(pg, "item"), l);
      if (cb_.options && !str(member(pg, "mode"), "item").empty() && !member(pg, "modes"))
        cb_.options(str(member(pg, "mode"), "item"), out, false);
    } else if (type == PageType::Cover) {   // its item's metadata (style, presets), a group's members: there when it opens
      std::map<std::string, appoh::Metadata> md;
      std::vector<appoh::Member> members;
      if (cb_.metadata && !str(pg, "item").empty())
        cb_.metadata(str(pg, "item"), md, false);
      if (cb_.members && !str(pg, "group").empty())
        cb_.members(str(pg, "group"), members, false);
      if (cb_.metadata && !str(pg, "group").empty())   // the group's presets
        cb_.metadata(str(pg, "group"), md, false);
    }
  }
}

/* The volume page (boards/nspanel/nextion/ohpanel_hmi.py volume_page()), over the media
   page that opened it (current_ stays that page): the volume between minus
   and plus, the slider, the mute button. */
void Engine::on_options(const std::string &item) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  // a group's members: its pages' rows made anew, the one shown drawn anew (its bindings hold the rows going)
  for (const cJSON *p = cJSON_IsArray(member(config_, "pages")) ? member(config_, "pages")->child : nullptr; p; p = p->next)
    if (str(p, "group") == item)
      if (const auto it = group_rows_.find(str(p, "id")); it != group_rows_.end()) {
        if (p == page)
          bindings_.clear();
        cJSON_Delete(it->second);
        group_rows_.erase(it);
      }
  if (page && str(page, "group") == item && shown_ == template_of(page)) {
    const cJSON *rows = rows_of(page);
    const int count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    sheet_ = std::min(sheet_, std::max(0, (count - 1) / rows_per_sheet(page)));
    draw_entities(page, shown_);
  }
  // a cover group's members: the rows drawn again where they are (no page anew: no flicker)
  if (page && type_of(page) == PageType::Cover && shown_ == "covers" && str(page, "group") == item) {
    drawn_.clear();
    bindings_.clear();
    if (cover_look_key(cover_look(page)) != covers_drawn_key_)
      show_physical("covers");   // the presets came or went: the page anew (the first row's place changes)
    else
      draw_covers(page);
  }
  // a cover's metadata: only if it changes what the page shows (its style, presets) -- the page anew then (its
  // pills where they are now); else only what it says
  if (page && type_of(page) == PageType::Cover && shown_ == "cover" && str(page, "item") == item) {
    if (cover_look_key(cover_look(page)) != cover_drawn_key_) {
      drawn_.clear();
      bindings_.clear();
      show_physical("cover");
    } else {
      cover_refresh(page);
    }
  }
  if (page && type_of(page) == PageType::MediaFavorites && str(member(page, "stations"), "item") == item &&
      shown_ == "buttons")
    draw_favorites(page);
  // a thermostat's range (its target's item) or its modes (the mode item's options) have come: the page anew (the
  // set of mode touch areas may change)
  if (page && type_of(page) == PageType::Thermostat && shown_ == "thermostat" &&
      (str(page, "item") == item || (str(member(page, "mode"), "item") == item && !member(page, "modes")))) {
    if (thermo_key(page) != thermo_drawn_key_) {
      drawn_.clear();
      bindings_.clear();
      show_physical("thermostat");
    } else {
      thermo_refresh(page);
    }
  }
  // a light's colour temperature range has come (its ends in openHAB): its slider anew
  if (page && type_of(page) == PageType::Light && str(member(page, "color_temperature"), "item") == item &&
      shown_ == "light")
    light_refresh(page);
}

/* A weather page's days: its "days", or the page itself (a page of one day, as before "days" came). */
void Engine::draw_dots() {
  const cJSON *cfg = current_ == "home" ? nullptr : page_config(current_);
  if (const PageType type = type_of(cfg); type == PageType::MediaFavorites || type == PageType::Entities ||
                                          type == PageType::Weather || (type == PageType::Cover && member(cfg, "group")))
    return;   // their dots are their sheets (draw_favorites, draw_entities, draw_covers)
  const auto page = pages_.find(shown_);
  if (page == pages_.end() || std::find(page->second.crop.begin(), page->second.crop.end(), "dots") ==
                                  page->second.crop.end())
    return;
  const std::string full = icon_glyph("circle-medium"), small = icon_glyph("circle-small");
  std::string dots;
  const std::vector<std::string> row = swipe_row();
  for (const std::string &id : row) {
    if (!dots.empty())
      dots += " ";
    dots += current_ == id ? full : small;
  }
  const bool in_row = row.size() > 1 && std::find(row.begin(), row.end(), current_) != row.end();
  txt("dots", in_row ? dots : "");
}

// ----------------------------------------------------------------- events --

void Engine::on_message(const nextion::Message &m) {
  using Kind = nextion::Message::Kind;
  // someone is using the panel (idle()) -- a slider moving, the colour wheel too; not a page that loaded (the
  // display reports every one, the night's black page too: it woke the screen again at once)
  const bool loaded = m.kind == Kind::Event && m.text.find("\"currentpage\"") != std::string::npos;
  if ((m.kind == Kind::Event && !loaded) || m.kind == Kind::Touch || m.kind == Kind::Point || m.kind == Kind::Value)
    touched_ms_ = now_ms_;
  if (m.kind == Kind::Value && shown_ == "volpage" && (m.name == "vols" || m.name == "volu")) {
    volume_slider(m.name == "volu", static_cast<int>(m.value));
    return;
  }
  if (note_drawn_) {
    // a notification on screen: a touch closes it and does nothing else
    bool load = false;
    if (m.kind == Kind::Event) {
      cJSON *e = cJSON_Parse(m.text.c_str());
      load = str(e, "component") == "currentpage";
      cJSON_Delete(e);
    }
    if (!load) {
      if ((m.kind == Kind::Point || m.kind == Kind::Touch) && !m.press)
        close_note();
      else if (m.kind == Kind::Event && m.text.find("release") != std::string::npos)
        close_note();
      return;
    }
  }
  switch (m.kind) {
    case Kind::Event: {
      cJSON *e = cJSON_Parse(m.text.c_str());
      if (e) {
        const cJSON *v = member(e, "value");
        std::string value = cJSON_IsString(v) ? v->valuestring : "";
        if (cJSON_IsNumber(v))
          value = std::to_string(v->valueint);
        on_event(str(e, "page"), str(e, "component"), value);
        cJSON_Delete(e);
      }
      break;
    }
    case Kind::Value:
      // the settings page's sliders, set0 .. set2
      if (!asleep_ && m.name.size() == 4 && m.name.rfind("set", 0) == 0 && m.name[3] >= '0' && m.name[3] <= '2')
        settings_value(m.name[3] - '0', static_cast<int>(m.value));
      else if (!asleep_ && shown_ == "light")   // the light's and the cover's (control_page)
        light_value(m.name, static_cast<int>(m.value));
      else if (!asleep_ && shown_ == "cover")
        cover_value(m.name, static_cast<int>(m.value));
      else if (!asleep_ && shown_ == "thermostat")
        thermo_value(m.name, static_cast<int>(m.value));
      break;
    default:
      break;
  }
}

/* A swipe ends with the release of whatever the finger is on when it lifts
   -- that release does nothing: one coming within kSwipeQuietMs after a swipe
   is dropped, and every other one waits release_wait_ms_ for a swipe that may
   still come (the display reports the two in either order). */
constexpr int64_t kSwipeQuietMs = 300;

void Engine::on_event(const std::string &page, const std::string &component, const std::string &value) {
  if (component == "currentpage") {
    on_page_loaded(page);
    return;
  }
  if (value == "press")   // a finger down: the drawing rests until its release has come (kPressRestMs)
    draw_rest_until_ms_ = now_ms_ + kPressRestMs;
  if (component == "swipe") {
    last_swipe_ms_ = now_ms_;
    pending_.reset();
  } else if (value == "release" && release_wait_ms_ > 0) {
    if (now_ms_ - last_swipe_ms_ < kSwipeQuietMs)
      return;   // the end of a swipe
    pending_ = Pending{page, component, value, now_ms_};
    return;
  }
  act(page, component, value);
}

void Engine::act(const std::string &page, const std::string &component, const std::string &value) {
  if (asleep_)
    return;  // the touch that wakes the screen (idle() does) presses nothing
  if (page == "settings" && component == "restart" && value == "release") {
    if (restart_armed_ && now_ms_ - restart_armed_ < 5000) {
      if (cb_.restart)
        cb_.restart();
    } else {
      restart_armed_ = now_ms_ ? now_ms_ : 1;
      txt("restart", settings_.texts.restart_confirm);
    }
    return;
  }
  if (component == "swipe") {
    swipe(value);
    return;
  }
  if (page == "netpage") {   // the network page: over the info page, which comes back when it closes
    if ((component == "close" || component == "back") && value == "release")
      show_current();
    return;
  }
  if (page == "system" && component == "card7" && value == "release") {
    show_physical("netpage");
    return;
  }
  if (page == "volpage") {   // the volume page: over the media page, which comes back when it closes
    if (component == "close")
      show_current();
    else
      volume_event(component);
    return;
  }
  if (component == "close") {
    if (value == "release")   // on letting go, as every touch
      back();
    return;
  }
  const cJSON *cfg = current_ == "home" ? nullptr : page_config(current_);

  if (page == "home") {
    if (component.rfind("shortcut", 0) == 0 && value == "release")
      act_shortcut(component[8] - '1');
    else if (component == "weather" && value == "release") {
      const std::string target = str(member(config_, "home"), "weather_page");
      if (!target.empty())
        open(target);
    }
    return;
  }
  if (!cfg)
    return;
  const std::string item = str(cfg, "item");

  if (page == "buttons" && component.rfind("tile", 0) == 0 && value == "release") {
    if (type_of(cfg) == PageType::MediaFavorites)
      favorites_event(cfg, component[4] - '1');
    else
      act_button(cfg, component[4] - '1');
  } else if (page == "cover" && value == "release") {
    cover_event(cfg, component);
  } else if (page == "covers" && value == "release") {
    covers_event(cfg, component);
  } else if (page == "thermostat" && value == "release") {
    thermo_event(cfg, component);
  } else if (page == "light" && component.rfind("side", 0) == 0 && component.size() == 5 && value == "release") {
    light_view_ = std::clamp(component[4] - '1', 0, 2);   // another view of the light: the page anew
    show_physical("light");
  } else if (page == "light" && component == "power" && value == "release") {
    light_power(cfg);
  } else if (page == "media") {
    media_event(cfg, component);
  }
}

/* A shortcut with an item follows its link only while the item is on --
   hidden while it is off (the usual), it is no target to touch by chance;
   one without an item always. */
void Engine::act_shortcut(int index) {
  const cJSON *s = slot(member(config_, "home"), "shortcuts", index);
  if (!s || !member(s, "open"))
    return;
  if (member(s, "item") && !on(s))
    return;
  open(str(s, "open"));
}

void Engine::act_button(const cJSON *page, int index) {
  const cJSON *b = slot(page, "buttons", index);
  if (!b)
    return;
  const std::string action = str(b, "action", member(b, "open") ? "open" : member(b, "item") ? "toggle" : "none");
  const std::string item = str(b, "item");
  if (action == "open") {
    open(str(b, "open"));
  } else if (action == "toggle" && cb_.command && !item.empty()) {
    cb_.command(item, toggle_command(b));
  } else if (action == "command" && cb_.command && !item.empty()) {
    cb_.command(item, str(b, "command"));
  }
}

/* A relay with an item switches at once and tells openHAB (ON/OFF), so the
   light works whether or not openHAB answers; openHAB switching the item
   switches the relay back (on_state). A press while openHAB is unreachable is
   kept: when the item's state arrives after the reconnect, the panel sends its
   own instead of following the stale one. "relay": false only commands the
   item. */
void Engine::hardware_button(int index) {
  touched_ms_ = now_ms_;  // someone is at the panel (idle())
  const cJSON *r = slot(member(config_, "home"), "relays", index);
  const std::string item = str(r, "item");
  const bool switches = !r || flag(r, "relay", true);
  // without openHAB nothing is sent (it would only fail): an item alone stays as it is, a relay's state
  // goes to openHAB once it is connected again (relay_pending_, follow_relays)
  if (!switches) {
    if (!item.empty() && cb_.command && status_.openhab)
      cb_.command(item, toggle_command(r));
    return;
  }
  relays_[index] = !relays_[index];
  if (cb_.relay)
    cb_.relay(index, relays_[index]);
  if (!item.empty()) {
    relay_pending_[index] = !status_.openhab;
    if (cb_.command && status_.openhab)
      cb_.command(item, relays_[index] ? "ON" : "OFF");
  }
  if (shown_ == "home")
    draw_relays();
}

void Engine::on_state(const std::string &item) {
  if (item == notify_item_)
    notify_state();
  screen_item(item);
  follow_relays(&item);
  for (const auto &b : bindings_)
    if (b.item == item)
      b.draw();
}

// ---------------------------------------------------------- notifications --

/* The notification item changed: a new text shows, an empty one (or NULL)
   takes the one shown away. The state the item has when it is first seen is
   an old one: it shows nothing. */
void Engine::notify_state() {
  appoh::ItemState st;
  if (!cb_.state || !cb_.state(notify_item_, st))
    return;
  const std::string value = st.undefined() ? "" : st.state;
  const bool first = !notify_known_;
  notify_known_ = true;
  if (value == notify_last_ && !first)
    return;
  notify_last_ = value;
  if (first)
    return;
  const bool was_drawn = note_drawn_;
  if (value.empty()) {
    if (note_)
      close_note();
    return;
  }
  Note n;
  n.what = parse_notification(value);
  n.colour = n.what.level == "alert" ? color565("red") : n.what.level == "warning" ? color565("yellow")
                                                                                   : color565("blue");
  note_ = n;
  touched_ms_ = now_ms_;  // wakes the screen (idle())
  if (was_drawn) {        // another one: the page again, then the new card
    note_drawn_ = false;
    show_physical(shown_);
  } else {
    draw_note();
  }
}

/* The words of `text` in lines of at most `width` pixels in `font`; a word
   wider than that on a line of its own. */
std::vector<std::string> Engine::wrap(int font, const std::string &text, int width) const {
  std::vector<std::string> lines;
  size_t from = 0;
  while (from <= text.size()) {
    const size_t nl = text.find('\n', from);
    const std::string para = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
    std::string line;
    size_t at = 0;
    while (at < para.size()) {
      const size_t sp = para.find(' ', at);
      const std::string word = para.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
      at = sp == std::string::npos ? para.size() : sp + 1;
      if (word.empty())
        continue;
      const std::string longer = line.empty() ? word : line + " " + word;
      if (!line.empty() && text_width(font, longer) > width) {
        lines.push_back(line);
        line = word;
      } else {
        line = longer;
      }
    }
    lines.push_back(line);
    if (nl == std::string::npos)
      break;
    from = nl + 1;
  }
  return lines;
}

/* `text` in at most `width` pixels of `font`: what does not fit makes way
   for "..."; `cut` adds them in any case (more text follows). */
std::string Engine::shorten(int font, const std::string &text, int width, bool cut) const {
  if (!cut && text_width(font, text) <= width)
    return text;
  std::string t = text;
  while (!t.empty() && (text_width(font, t + "...") > width || t.back() == ' ')) {
    size_t k = t.size() - 1;  // the last character, a whole UTF-8 one
    while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xC0) == 0x80)
      k--;
    t.erase(k);
  }
  return t + "...";
}

/* The card: rounded, framed in the level's colour -- wide on the left --
   which is the title's too, the text below; as high as it needs, near the
   top of the screen.
   Drawn once a page has settled, never on the boot or the black page. */
void Engine::draw_note() {
  if (!note_ || note_drawn_ || !started_ || asleep_ || status_.setup || !expected_.empty() || shown_.empty() ||
      shown_ == "boot" || shown_ == "blank")
    return;
  constexpr int kVisibleW = 450;  // the panel's frame covers the rest (boards/nspanel/nextion/layout.py VISIBLE_W)
  constexpr int kW = 400, kX = (kVisibleW - kW) / 2, kR = 12, kPad = 14, kTitleH = 44, kLineH = 28, kMaxH = 296, kTop = 24;
  constexpr int kLeft = 14, kEdge = 2;  // the frame: on the left, elsewhere
  constexpr int kTitleFont = 4, kTextFont = 3, kCard = 8452, kText = 65535;
  const Note &n = *note_;
  constexpr int kInX = kX + kLeft + 14;  // the texts' left edge
  const int inner = kX + kW - kEdge - 12 - kInX;
  const int title_h = n.what.title.empty() ? 0 : kTitleH;
  std::vector<std::string> lines = wrap(kTextFont, n.what.text, inner);
  const size_t room = static_cast<size_t>((kMaxH - 2 * kPad - title_h) / kLineH);
  const bool more = lines.size() > room;
  if (more)
    lines.resize(room);
  for (size_t i = 0; i < lines.size(); i++)  // a word too long for a line, the last line of a longer text
    lines[i] = shorten(kTextFont, lines[i], inner, more && i + 1 == lines.size());
  const int h = 2 * kPad + title_h + static_cast<int>(lines.size()) * kLineH;
  const int y = std::min(kTop, (320 - h) / 2);
  const auto put = [this](const std::string &c) {
    if (link_)
      sink_.send(c);
  };
  rounded(kX, y, kW, h, kR, n.colour, true);   // past send(): the card is up while it is drawn anew
  rounded(kX + kLeft, y + kEdge, kW - kLeft - kEdge, h - 2 * kEdge, kR - kEdge, kCard, true);
  const auto xstr = [&](int yy, int hh, int font, int colour, const std::string &text) {
    put("xstr " + std::to_string(kInX) + "," + std::to_string(yy) + "," + std::to_string(inner) + "," +
        std::to_string(hh) + "," + std::to_string(font) + "," + std::to_string(colour) + ",0,0,1,3,\"" +
        nextion::quote(text) + "\"");
  };
  int at = y + kPad;
  if (title_h) {
    xstr(at, 40, kTitleFont, n.colour, shorten(kTitleFont, n.what.title, inner, false));
    at += title_h;
  }
  for (const auto &l : lines) {
    xstr(at, kLineH + 2, kTextFont, kText, l);
    at += kLineH;
  }
  put("tsw 255,0");  // the page's objects take no touch under the card
  put("sendxy=1");   // ... a touch anywhere is reported instead: it closes the card
  note_drawn_ = true;
}

/* A touch on the card, or the item emptied: the page as it is now. */
void Engine::close_note() {
  note_.reset();
  if (!note_drawn_)
    return;
  note_drawn_ = false;
  if (link_)
    sink_.send("sendxy=0");
  if (!shown_.empty())
    show_physical(shown_);
}

/* Relays that follow an item switch even while another page is shown; those
   following `item`, or all with nullptr. An item that is NULL or UNDEF (openHAB
   restarting, before its persistence restores it) switches nothing: the relay
   stays as it is, and openHAB, once connected, is told its state -- once. */
void Engine::follow_relays(const std::string *item) {
  for (int i = 0; i < 2; i++) {
    const cJSON *r = slot(member(config_, "home"), "relays", i);
    if (!r || (item && str(r, "item") != *item) || !flag(r, "relay", true) || !has_state(r))
      continue;
    if (state_of(r).undefined()) {
      if (status_.openhab && !relay_told_[i] && cb_.command) {
        relay_told_[i] = true;
        relay_pending_[i] = false;
        cb_.command(str(r, "item"), relays_[i] ? "ON" : "OFF");
      }
      continue;
    }
    relay_told_[i] = false;
    const bool v = on(r);
    if (relay_pending_[i]) {
      relay_pending_[i] = false;
      if (v != relays_[i] && cb_.command)
        cb_.command(str(r, "item"), relays_[i] ? "ON" : "OFF");
    } else if (v != relays_[i]) {
      relays_[i] = v;
      if (cb_.relay)
        cb_.relay(i, v);
    }
  }
}

void Engine::redraw_states() {
  follow_relays(nullptr);
  for (const auto &b : bindings_)
    b.draw();
}

void Engine::idle(int64_t now_ms) {
  now_ms_ = now_ms;
  cover_tick();
  thermo_tick();
  if (slider_resync_ && !slider_held()) {   // the finger off the slider a while: it shows what the item says
    slider_resync_ = false;
    if (const cJSON *page = current_ == "home" ? nullptr : page_config(current_)) {
      if (shown_ == "light")
        light_refresh(page);
    }
  }
  if (start_at_ms_ && now_ms >= start_at_ms_) {   // the boot page has been there long enough (start())
    start_at_ms_ = 0;
    if (shown_ == "boot" && !status_.setup)
      show_current();
  }
  if (pending_ && now_ms - pending_->at >= release_wait_ms_) {   // no swipe came: the release counts
    const Pending p = *pending_;
    pending_.reset();
    act(p.page, p.component, p.value);
  } else if (draw_rest_until_ms_ - now_ms > kPressRestMs) {
    draw_rest_until_ms_ = now_ms;   // the clock went back (the host tests'): no rest from the future
  } else if (!pending_ && now_ms >= draw_rest_until_ms_ && (!logo_queue_.empty() || cover_job_)) {
    draw_next_logo();   // not while a touch waits: it comes first
    if (logo_queue_.empty())
      draw_art_step();
    draw_rest_until_ms_ = now_ms + kDrawRestMs;   // then a rest: the display can report a touch
  }
  fade_tick();
  if (wheel_pending_ && now_ms - wheel_sent_ms_ >= kSlideMs)
    send_wheel(true);
  if (colour_after_on_ && now_ms >= colour_after_on_->until_ms)
    colour_after_on_.reset();   // the light did not come on: the colour is not sent later by surprise
  if (prefetch_at_ >= 0 && now_ms >= prefetch_at_) {
    prefetch_at_ = -1;
    prefetch_options();
  }
  if (touched_ms_ < 0)
    touched_ms_ = now_ms;
  // a player that plays stays (and the volume card over it): what plays is what one looks at
  const cJSON *shown_page = current_ == "home" ? nullptr : page_config(current_);
  const bool playing = shown_page && type_of(shown_page) == PageType::Media && media_playing(shown_page);
  if (current_ == "home" || asleep_ || note_ || playing)
    home_ms_ = now_ms;  // (not away from a notification either)  // the time on another page counts from leaving home at the earliest
  else if (settings_.home_after > 0 && started_ &&
           now_ms - std::max(touched_ms_, home_ms_) >= settings_.home_after * 1000LL) {
    home_ms_ = now_ms;
    open("home", false);
  }
  screen_tick();
  // numbers from items go into the stored configuration once they settle
  if (!remember_.empty() && now_ms - remember_at_ >= 5000) {
    if (cb_.remember)
      for (const auto &[key, value] : remember_)
        cb_.remember(key, value);
    remember_.clear();
    const std::map<std::string, const std::string *> items = {{"brightness", &screen_.brightness_item},
                                                              {"idle_brightness", &screen_.idle_brightness_item},
                                                              {"idle_after", &screen_.idle_after_item}};
    if (cb_.command)
      for (const auto &[key, value] : command_)
        if (!items.at(key)->empty())
          cb_.command(*items.at(key), std::to_string(value));
    command_.clear();
  }
}

// ----------------------------------------------------------------- screen --

void Engine::show_current() {
  if (asleep_)
    show_physical("blank");
  else
    show_physical(current_ == "home" ? "home" : template_of(page_config(current_)));
}

void Engine::set_level(int level) {
  level = std::clamp(level, 0, 100);
  if (level == level_)
    return;
  level_ = level;
  send("dim=" + std::to_string(level));
}

bool Engine::night_now() const {
  if (!screen_.night_item.empty()) {
    appoh::ItemState s;
    return cb_.state && cb_.state(screen_.night_item, s) && s.state == "ON";
  }
  if (screen_.night_from < 0 || now_ < 1000000000)  // no window, or no time yet
    return false;
  struct tm t;
  localtime_r(&now_, &t);
  return in_night(t.tm_hour * 60 + t.tm_min, screen_.night_from, screen_.night_to);
}

/* In use: the brightness; idle_after s without a touch: the idle one; at
   night, or idle at 0 %: off -- the black page, so the touch that wakes it
   presses nothing; it opens home. */
void Engine::screen_tick() {
  // nothing while the display is away (rewritten, restarting): a command in an
  // upload's data breaks it
  if (!started_ || shown_.empty())
    return;
  const ScreenSettings &s = screen_;
  const int64_t quiet = now_ms_ - touched_ms_;
  const bool idle = s.idle_after > 0 && quiet >= s.idle_after * 1000LL;
  bool off = false;
  int level = s.brightness;
  if (night_now())
    off = quiet >= (s.idle_after > 0 ? s.idle_after : 60) * 1000LL;
  else if (idle) {
    level = s.idle_brightness;
    off = level <= 0;
  }
  if (off) {
    if (!asleep_) {
      asleep_ = true;
      set_level(0);
      show_physical("blank");
    }
    return;
  }
  if (asleep_) {
    asleep_ = false;
    current_ = "home";
    history_.clear();
    show_physical("home");
  }
  set_level(level);
}

/* An item of display.screen changed in openHAB: the panel takes the number
   (openHAB wins) and keeps it in its configuration once it settles. */
void Engine::screen_item(const std::string &item) {
  appoh::ItemState st;
  if (item.empty() || !cb_.state || !cb_.state(item, st) || !st.has_numeric || st.undefined())
    return;
  const int v = static_cast<int>(st.numeric + 0.5);
  struct Key { const std::string &item; int &value, &stored; const char *name; int low, high; };
  for (const Key k : {Key{screen_.brightness_item, screen_.brightness, settings_.screen.brightness, "brightness", 1, 100},
                      Key{screen_.idle_brightness_item, screen_.idle_brightness, settings_.screen.idle_brightness,
                          "idle_brightness", 0, 100},
                      Key{screen_.idle_after_item, screen_.idle_after, settings_.screen.idle_after, "idle_after", 0, 3600}}) {
    if (k.item != item)
      continue;
    k.value = std::clamp(v, k.low, k.high);
    if (k.value != k.stored) {
      k.stored = k.value;
      remember_[k.name] = k.value;
      remember_at_ = now_ms_;
    }
  }
}

/* A configuration: its screen numbers -- but where an item is linked, what
   openHAB says (and that is kept in the configuration too), unless the
   number was just changed here (the web interface): that goes to the item. */
void Engine::screen_config(const ScreenSettings &before) {
  ScreenSettings &stored = settings_.screen;
  screen_ = stored;
  struct Key { const std::string &item; int &value, &stored; int was; const std::string &was_item; const char *name; };
  for (const Key k : {Key{stored.brightness_item, screen_.brightness, stored.brightness, before.brightness,
                          before.brightness_item, "brightness"},
                      Key{stored.idle_brightness_item, screen_.idle_brightness, stored.idle_brightness,
                          before.idle_brightness, before.idle_brightness_item, "idle_brightness"},
                      Key{stored.idle_after_item, screen_.idle_after, stored.idle_after, before.idle_after,
                          before.idle_after_item, "idle_after"}}) {
    if (k.item.empty())
      continue;
    appoh::ItemState st;
    const bool known = cb_.state && cb_.state(k.item, st) && st.has_numeric && !st.undefined();
    const int item_value = known ? static_cast<int>(st.numeric + 0.5) : k.stored;
    // changed here with the same item linked before: the web interface's -- else openHAB wins
    if (screen_known_ && k.item == k.was_item && k.stored != k.was) {
      if (cb_.command && item_value != k.stored)
        cb_.command(k.item, std::to_string(k.stored));
    } else if (known && item_value != k.stored) {
      k.value = k.stored = item_value;
      remember_[k.name] = item_value;
      remember_at_ = now_ms_;
    }
  }
  screen_known_ = true;
}

void Engine::tick(time_t now) {
  const bool new_minute = now / 60 != now_ / 60;
  const bool new_second = now != now_;
  now_ = now;
  if (new_minute)
    draw_clock();
  /* Rules by a date match by the time of day too ("from", "until") and by the day (after midnight): what shows an
     item holding a date drawn again each minute -- unchanged, nothing goes to the display. */
  if (new_minute && dated_rules_) {
    const std::vector<Binding> now_bound = bindings_;
    for (const auto &b : now_bound) {
      appoh::ItemState st;
      if (cb_.state && cb_.state(b.item, st) && state_epoch(st) >= 0)
        b.draw();
    }
  }
  // the info page follows uptime, memory and temperature while it is shown
  if (new_second && current_ != "home")
    if (const cJSON *page = page_config(current_); page && type_of(page) == PageType::System && shown_ == template_of(page))
      draw_system(page);
  if (new_second && shown_ == "netpage" && now % 5 == 0)   // the addresses change seldom
    draw_network_page();
}

}  // namespace appdisplay

