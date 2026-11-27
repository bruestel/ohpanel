/*
   The boot page and home (Engine::draw_boot, draw_home): the clock, the weather, the values, the relays.
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Cover.h"
#include "appdisplay/Icons.h"
#include "EngineDetail.h"

#include <qrcodegen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace appdisplay {
using namespace detail;

// ---------------------------------------------------------------- drawing --

/* The labels have room for 8 characters (their txt_maxl), hence no
   colons; the texts in the display language. */
void Engine::draw_boot() {
  const Texts &t = settings_.texts;
  if (const int logo = picture("logo", "boot"); logo >= 0)   // the logo on the background in use
    num("logo_pic", "pic", logo);
  vis("logo_pic", true);   // hidden while the page loads: its design's logo is on the default background
  if (status_.hotspot)
    show_frames("status", {t.boot_setup}, 0);
  else  // starting: dots running after the title
    show_frames("status", {t.boot_starting, t.boot_starting + " .", t.boot_starting + " . .", t.boot_starting + " . . ."},
                400);
  txt("firmware_text", "Firmware");
  txt("firmware_value", status_.version);  // the field ends short of the hotspot QR code
  if (status_.hotspot) {
    // No network to join yet: how to reach the setup page -- the hotspot is
    // open, so its name and the code are all it takes
    txt("device_text", t.boot_wifi);
    txt("device_value", status_.hotspot_ssid);
    txt("ip_text", t.boot_address);
    txt("ip_value", "http://" + status_.ip);
    txt("ipv6_text", "");
    txt("ipv6_value", "");
    txt("info_text", "");
    txt("info_value", "");   // the code speaks for itself
    // scanned with a phone, joins the hotspot; the setup page is then at http://192.168.4.1
    // drawn by the firmware as on the QR page (draw_code): dots in the accent, small error correction --
    // the fewest modules, as large as fit
    draw_code("qr_area", wifi_qr_text(status_.hotspot_ssid, ""), "low", true, theme_.accent, 5);
  } else {
    txt("device_text", t.boot_device);
    txt("device_value", status_.device);
    // both rows always, "–" for an address there is none of
    txt("ip_text", "IPv4");
    txt("ip_value", status_.ip.empty() ? t.undefined : status_.ip);
    txt("ipv6_text", "IPv6");
    txt("ipv6_value", status_.ipv6.empty() ? t.undefined : status_.ipv6);
    txt("info_text", "openHAB");
    txt("info_value", status_.openhab ? t.boot_connected : t.boot_connecting);
    if (const auto it = drawn_.find("qr_area"); it != drawn_.end() && !it->second.empty())
      show_physical("boot");   // the hotspot's code gone with the hotspot: the page anew, without it
  }
}

/* The large clock with AM/PM (draw_clock): the time centred in the clock's field, AM/PM 6 px after its last digit --
   level with the digits' top, both drawn here (the field itself hidden: the display repainted it whole on a change, AM/PM with it, a flicker
   each minute), each only as wide as it is, on the page's background as it is there. A change of the hour's
   digits (twice a day) clears where the two were. */
void Engine::draw_big_clock(const std::string &time, const std::string &meridiem) {
  const auto bx = boxes_.find(shown_);
  const auto f = fit_.find(shown_);
  if (bx == boxes_.end() || !bx->second.count("clock") || !bx->second.count("ampm") || f == fit_.end() ||
      !f->second.count("clock"))
    return;
  const Box &c = bx->second.at("clock"), &m = bx->second.at("ampm");
  const int font = f->second.at("clock").font;
  const int digits = text_width(font, time), left = c.x + (c.w - digits) / 2, x = left + digits + 6;
  // AM/PM as far as the relays' labels at the side (or the screen's edge): a two-digit hour's ends past the field
  int right = display_size_.visible_w;
  for (const char *label : {"relay1_label", "relay2_label"})
    if (const auto r = bx->second.find(label); r != bx->second.end() && r->second.x > x)
      right = std::min(right, r->second.x);
  /* where even the widest time ("00:00") leaves no room for it there (the NSPanel US upright: 116 px digits), AM/PM
     stands where the design has it, above the clock to the right -- in the one place all day */
  const int need = text_width(2, meridiem) + 4, widest = text_width(font, "00:00");
  const bool beside = c.x + (c.w - widest) / 2 + widest + 6 + need <= right;
  const int mx = beside ? x : m.x, my = beside ? c.y : m.y;
  const int mw = beside ? std::max(0, std::min(right - x, need)) : m.w;
  const std::string key = std::to_string(left) + "," + std::to_string(digits) + "," + std::to_string(beside ? mw : 0) + "|" + time +
                          "|" + meridiem;
  std::string &drawn = drawn_["big_clock"];
  if (drawn == key)
    return;
  const auto ground = [this](int xcen, int ycen) {
    return bg_sent_ >= 0 ? std::to_string(bg_sent_) + "," + std::to_string(xcen) + "," + std::to_string(ycen) + ",0"
                         : "0," + std::to_string(xcen) + "," + std::to_string(ycen) + ",3";
  };
  // where the two were, if the time takes another place now: cleared
  int was_left = 0, was_end = 0;
  const bool moved = !drawn.empty() && std::sscanf(drawn.c_str(), "%d,", &was_left) == 1 && was_left != left;
  if (moved) {
    int was_digits = 0, was_mw = 0;
    std::sscanf(drawn.c_str(), "%d,%d,%d", &was_left, &was_digits, &was_mw);
    was_end = std::min(right, was_left + was_digits + 6 + was_mw);
    const int from = std::min(was_left, left), to = std::max(was_end, x + mw);
    send("xstr " + std::to_string(from) + "," + std::to_string(c.y) + "," + std::to_string(to - from) + "," +
         std::to_string(c.h) + ",2,0," + ground(0, 1) + ",\"\"");
  }
  drawn = key;
  send("xstr " + std::to_string(left - 1) + "," + std::to_string(c.y) + "," + std::to_string(digits + 2) + "," +
       std::to_string(c.h) + "," + std::to_string(font) + "," + std::to_string(theme_.text) + "," + ground(1, 1) +
       ",\"" + nextion::quote(time) + "\"");
  if (mw > 0)
    // beside: its top level with the digits' (both fonts' ink starts 4 rows into the cell: fonts.py,
    // zifont.cap_rows); above: as the design's field, at its bottom
    send("xstr " + std::to_string(mx) + "," + std::to_string(my) + "," + std::to_string(mw) + "," +
         std::to_string(m.h) + ",2," + std::to_string(theme_.text) + "," + ground(beside ? 0 : 0, beside ? 0 : 2) + ",\"" +
         nextion::quote(meridiem) + "\"");
}

/* The large clock, once the page's refresh is on again (draw_clock). */
void Engine::meridiem_tick() {
  if (meridiem_pending_.empty() || refresh_paused_)
    return;
  if (shown_ == home_page())
    draw_big_clock(clock_pending_, meridiem_pending_);
  meridiem_pending_.clear();
}

std::string Engine::home_page() const {
  return str(member(config_, "home"), "style") == "clock" && pages_.count("home_clock") ? "home_clock" : "home";
}

void Engine::draw_clock() {
  if (!home_shown() || now_ == 0)
    return;
  struct tm t;
  localtime_r(&now_, &t);
  const bool ampm = settings_.time_format.find("%p") != std::string::npos;
  std::string time_format = settings_.time_format;
  if (ampm) {  // the design shows AM/PM in a field of its own
    const size_t p = time_format.find("%p");
    time_format.erase(p, 2);
    while (!time_format.empty() && time_format.back() == ' ')
      time_format.pop_back();
  }
  /* AM/PM sits above the minutes, its field reaching into the clock's, which
     repaints all of its field when it changes: both are drawn again together
     -- with refreshing paused, the display draws them in their order, AM/PM
     last. Only with a 12-hour clock; hidden otherwise. */
  const auto bx = boxes_.find(shown_);
  const bool placed = ampm && bx != boxes_.end() && bx->second.count("clock") && bx->second.count("ampm");
  if (placed) {
    /* The large clock (home.style "clock") with AM/PM: drawn by the firmware (draw_big_clock), the design's fields
       hidden -- once the page's refresh is on again (meridiem_tick, from idle()). */
    vis("ampm", false);
    vis("clock", false);
    clock_pending_ = format_time(time_format, t, settings_.texts);
    meridiem_pending_ = format_time("%p", t, settings_.texts);
    if (!refresh_paused_)
      meridiem_tick();
  } else {
    const bool pause = ampm && !refresh_paused_;
    if (pause)
      send("ref_stop");
    vis("clock", true);   // (hidden while the large clock had AM/PM)
    txt("clock", format_time(time_format, t, settings_.texts));
    vis("ampm", ampm);
    if (ampm) {
      sent_.erase("ampm.txt");   // drawn again even if unchanged
      txt("ampm", format_time("%p", t, settings_.texts));
    }
    if (pause)
      send("ref_star");
  }
  /* A date wider than its field (long names: "%A, %e %B") in a smaller font first, two
     sizes down at most (26, 22, 18 px), only then shortened with "..." */
  const std::string date = format_time(settings_.date_format, t, settings_.texts);
  const int font = font_to_fit(shown_, "date", date, 2);
  if (font >= 0)
    num("date", "font", font);   // the design's again when the date fits it (the page loaded: sent anew)
  txt("date", date, font);  // dimmed by the page (th_dim)
}

void Engine::draw_relays() {
  if (!expected_.empty())
    return;   // a page on its way: home's labels drawn as it loads (upright ones would land on the next page)
  const cJSON *home = member(config_, "home");
  // upright labels (the NSPanel US on its side: the design marks where, the manifest's "boxes")
  const auto pg = boxes_.find(shown_);   // home's, or the clock style's
  const bool upright = pg != boxes_.end() && pg->second.count("relay1_label");
  for (int i = 0; i < 2; i++) {
    const cJSON *r = slot(home, "relays", i);
    const std::string side = "relay" + std::to_string(i + 1);
    if (!r || flag(r, "hidden")) {   // "hidden": it works unseen
      vis(side + "_text", false);
      vis(side + "_bar", false);
      if (upright)
        relay_label(pg->second.at(side + "_label"), side, "", theme_.text);
      continue;
    }
    // the relay where the button switches it (at once) or has no item, else the button's item
    const bool is_on = member(r, "item") && relay_ref(r) != r ? on(r) : relays_[i];
    // its label: an item's state ("text", forum #181) while it has one (not NULL, UNDEF, empty), else the fixed one
    const cJSON *text = member(r, "text");
    std::string label = has_state(text) && !state_of(text).undefined() ? value_of(text) : "";
    if (label.empty())
      label = str(r, "label");
    if (upright) {
      vis(side + "_text", false);
      relay_label(pg->second.at(side + "_label"), side, label, i == 0 ? theme_.text : theme_.dim);
    } else {
      txt(side + "_text", label);
    }
    // the bar under it (boards/nspanel/nextion/ohpanel_hmi.py home_page): the accent while on
    num(side + "_bar", "bco", is_on ? theme_.accent : theme_.tile);
  }
}

/* A relay's label upright in `b`, read from the bottom up: its characters in the turned font (font 1's Latin-1 turned
   a quarter anticlockwise, boards/nspanel/nextion/fonts.py), one over the other, each its advance in font 1 above the
   one before, the whole centred on the box -- drawn again only when it changes, the background back first. Each
   on the background cropped (as the EU's label fields): on nothing, the display smooths the edges against black. */
void Engine::relay_label(const Box &b, const std::string &side, const std::string &text, int colour) {
  constexpr int kTurnedFont = 10, kCharW = 18, kCharH = 16;
  const std::string label = shorten(1, text, b.h, false);
  const std::string key = label + "|" + std::to_string(colour);
  if (drawn_[side + "_label"] == key)
    return;
  drawn_[side + "_label"] = key;
  restore(b.x, b.y, b.w, b.h);
  int bottom = b.y + (b.h + text_width(1, label)) / 2;   // where the next character's ink ends below
  for (size_t at = 0; at < label.size();) {
    size_t n = 1;   // one character, a whole UTF-8 one
    while (at + n < label.size() && (static_cast<unsigned char>(label[at + n]) & 0xC0) == 0x80)
      n++;
    const std::string c = label.substr(at, n);
    // the character at the bottom of its cell; the cell's rest above it, which the next one draws over
    const std::string ground = bg_sent_ >= 0 ? std::to_string(bg_sent_) + ",0,0,0" : "0,0,0,3";
    send("xstr " + std::to_string(b.x + (b.w - kCharW) / 2) + "," + std::to_string(bottom - kCharH) + "," +
         std::to_string(kCharW) + "," + std::to_string(kCharH) + "," + std::to_string(kTurnedFont) + "," +
         std::to_string(colour) + "," + ground + ",\"" + nextion::quote(c) + "\"");
    bottom -= text_width(1, c);
    at += n;
  }
}

void Engine::draw_home() {
  const cJSON *h = member(config_, "home");
  const bool clock = shown_ == "home_clock";   // the clock style: no value rows, no info lines, a row of shortcuts
  send("vis 255,1");  // the page hides everything while it loads
  hide_busy();  // all but that
  draw_clock();
  draw_wifi();

  // without an item in the configuration, the temperatures and the weather icon do not show
  // the value rows: an icon and a value each; the first may come as
  // indoor_temperature, its older name, with the house as its icon
  for (int i = 0; i < 2 && !clock; i++) {
    const cJSON *v = slot(h, "values", i);
    const bool legacy = !v && i == 0 && member(h, "indoor_temperature");
    if (legacy)
      v = member(h, "indoor_temperature");
    const std::string row = i ? "row2" : "row1";
    const std::string icon = row + "_icon";
    vis(icon, v != nullptr);
    if (!v) {
      show_number(row + "_u", false);
      continue;
    }
    // a matching value_style rule: another icon and its colour, the value's colour, blinking, another format
    bind(v, [this, v, row, icon, legacy] {
      const RuleStyle style = style_of(v);
      if (style.icon_color >= 0)
        num(icon, "pco", style.icon_color);
      else if (member(v, "color"))
        num(icon, "pco", color_of(v, "color", theme_.dim));
      else
        page_colour(icon, theme_.dim);
      // blinking: the icon with the value
      show_text(icon, icon_glyph(!style.icon.empty() ? style.icon.c_str()
                                 : legacy           ? "home-thermometer-outline"
                                                    : icon_name(v, str(v, "icon")).c_str()),
                style.blink);
      std::string number, unit;
      number_and_unit(v, number, unit, &style);
      show_number(row + "_u", true, number, unit, style.color, style.blink);
    });
  }
  draw_home_weather(h, true);

  for (int i = 0; i < 2; i++)
    if (const cJSON *r = slot(h, "relays", i)) {
      bind(r, [this] { draw_relays(); });
      if (cJSON_IsObject(member(r, "relay")))   // an item of its own
        bind(member(r, "relay"), [this] { draw_relays(); });
      if (cJSON_IsObject(member(r, "text")))    // its label from an item
        bind(member(r, "text"), [this] { draw_relays(); });
    }
  draw_relays();

  // status icons: top1 .. top10 (the bar under a relay's label shows its state)
  for (int i = 0; i < 10; i++) {
    const cJSON *s = slot(h, "status_icons", i);
    const std::string obj = "top" + std::to_string(i + 1);
    if (!s) {
      txt(obj, "");
      continue;
    }
    bind(s, [this, s, obj] {
      if (shown(s)) {
        show_icon(obj, s, color_of(s, "color", theme_.text));
      } else {
        anims_.erase(obj);
        txt(obj, "");
      }
    });
  }

  // shortcuts: short1_icon .. short12_icon, three rows of four numbered as read; the clock style's row
  if (clock)
    draw_home_shortcuts(h);
  for (int i = 0; i < 12 && !clock; i++) {
    const cJSON *s = slot(h, "shortcuts", i);
    const std::string obj = "short" + std::to_string(i + 1) + "_icon";
    if (!s) {
      txt(obj, "");
      continue;
    }
    bind(s, [this, s, obj] {
      if (shown(s)) {
        show_icon(obj, s, color_of(s, "color", theme_.text));
      } else {
        anims_.erase(obj);
        txt(obj, "");
      }
    });
  }

  // info lines: infoN_tag (left), infoN_text (main), infoN_right (right)
  for (int line = 0; line < 2 && !clock; line++) {
    const cJSON *l = slot(h, "info", line);
    const std::string n = "info" + std::to_string(line + 1);
    const char *fields[3] = {"left", "main", "right"};
    const std::string objs[3] = {n + "_tag", n + "_text", n + "_right"};
    /* The main text has the whole line while the right field is empty ("_wide", its place too): two fields, as a
       field's width is fixed, one showing -- and the right one hidden, or it would repaint over the wide one. Each
       leaves before the other shows: hiding a field repaints what is under it. */
    const cJSON *right = member(l, "right");
    const std::string wide = n + "_wide";
    std::function<void()> draw_main;
    for (int f = 0; f < 3; f++) {
      const cJSON *ref = member(l, fields[f]);
      /* The left field also comes as an icon ("_tagicon") and both as a badge
         (on a picture of a rounded box in a colour, per background: "_badge",
         "_badgeico"), see boards/nspanel/nextion/ohpanel_hmi.py, home_page; one shows. A
         badge only with a value: an empty one shows the plain field. */
      const std::string plain = objs[f], ico = n + "_tagicon";
      const auto badge_of = [n](const std::string &base) {
        return n + (base == n + "_tag" ? "_badge" : "_badgeico");
      };
      std::vector<std::string> variants = {plain};
      if (f == 0)
        variants = {plain, badge_of(plain), ico, badge_of(ico)};
      else if (f == 1)
        variants = {plain, wide};
      const bool icon = f == 0 && member(ref, "icon"), bg = f == 0 && member(ref, "bg");
      const int fg = color_of(ref, "color", theme_.text);
      // the badge's picture by its palette colour's name (composed per background: yellow is orange on White), a
      // colour of its own (#rrggbb, theme-*) by the palette colour nearest to it
      const std::string bg_name = str(ref, "bg");
      const bool named = !bg_name.empty() && bg_name[0] != '#' && bg_name.rfind("theme-", 0) != 0;
      const std::string badge_pic =
          bg ? std::to_string(picture("badge" + std::to_string(line + 1),
                                      named ? bg_name : nearest_palette(color_of(ref, "bg", 0))))
             : "";
      const bool coloured = member(ref, "color") != nullptr;
      const std::function<void()> draw = [this, ref, variants, plain, ico, icon, bg, fg, coloured, badge_pic, f, badge_of,
                                           right, wide, draw_main] {
        const RuleStyle style = style_of(ref);
        const std::string value = !ref ? "" : icon ? glyph(ref) : value_of(ref, nullptr, &style);
        const RuleStyle right_style = style_of(right);
        const bool free_line = f == 1 && (!right || value_of(right, nullptr, &right_style).empty());
        const std::string base = icon ? ico : free_line ? wide : plain;
        const std::string obj = bg && !value.empty() && badge_pic != "-1" ? badge_of(base) : base;
        if (f == 2 && !value.empty())
          draw_main();
        for (const auto &v : variants)
          vis(v, v == obj && (f != 2 || !value.empty()));
        if (f == 2 && value.empty())
          draw_main();
        if (obj != base)
          num(obj, "pic", atoi(badge_pic.c_str()));
        // a colour of a matching value_style rule, or its own; else the page's: text, the right
        // field (a time, mostly) dimmed (th_*)
        if (style.color >= 0)
          num(obj, "pco", style.color);
        else if (obj != base && !coloured && theme_.light)
          num(obj, "pco", kWhite);   // on a badge white, also where the text is black (a light background)
        else if (coloured || obj != base || icon)
          num(obj, "pco", fg);
        else
          page_colour(obj, f == 2 ? theme_.dim : theme_.text);
        for (const auto &v : variants)
          if (v != obj)
            anims_.erase(v);
        show_text(obj, value, style.blink);
      };
      if (f == 1)
        draw_main = draw;
      bind(ref, draw);
    }
  }
}



/* Home's weather block: the temperature now, today's low and high under it, the weather icon -- each value its own
   item (bound: drawn again as it changes), or today's of the weather feed ("weather_feed", EngineFeed.cpp), which has no
   item to bind and is drawn again by weather_feed_state as the feed changes (`bound` false: nothing bound anew). Every
   field said shown or hidden each time, so that drawing it again over itself leaves nothing behind. */
void Engine::draw_home_weather(const cJSON *h, bool bound) {
  const auto draw = [this, bound](const cJSON *ref, std::function<void()> f) {
    if (bound)
      bind(ref, std::move(f));
    else
      f();
  };
  if (shown_ == "home_clock") {   // the clock style: the icon of the icon font, the temperature in one field
    const cJSON *outdoor = home_weather(h, "outdoor_temperature");
    if (!outdoor)
      txt("outdoor", "");
    else
      draw(outdoor, [this, outdoor] {
        std::string number, unit;
        number_and_unit(outdoor, number, unit);
        txt("outdoor", unit.rfind("°", 0) == 0 ? number + "°" : unit.empty() ? number : number + " " + unit);
      });
    const cJSON *weather = home_weather(h, "weather_icon");
    vis("weather_icon", weather != nullptr);
    vis("weather_hs", weather != nullptr || outdoor != nullptr);
    if (weather)
      draw(weather, [this, weather] {
        txt("weather_icon", icon_glyph(weather_glyph(weather_icon(weather)).c_str()));
        page_colour("weather_icon", theme_.text);
      });
    return;
  }
  const cJSON *outdoor = home_weather(h, "outdoor_temperature");
  if (!outdoor)
    show_number("outdoor_u", false);
  else
    draw(outdoor, [this, outdoor] {
      std::string number, unit;
      number_and_unit(outdoor, number, unit);
      // as weather apps show it: "°" without the scale, in the number's own size and colour (its wide
      // field); another unit small after it
      if (unit.rfind("°", 0) == 0)
        show_number("outdoor_u", true, number + "°", "");
      else
        show_number("outdoor_u", true, number, unit);
    });
  // today's low and high under it
  draw_range(h);
  if (bound)
    for (const char *key : {"outdoor_low", "outdoor_high"})
      if (const cJSON *r = home_weather(h, key))
        bind(r, [this, h] { draw_range(h); });
  const cJSON *weather = home_weather(h, "weather_icon");
  vis("weather_pic", weather != nullptr);
  vis("weather_hs", weather != nullptr);
  if (weather)
    draw(weather, [this, weather] {
      const int pic = weather_pic(weather, "weather_home");
      if (pic >= 0) num("weather_pic", "pic", pic);
    });
}

/* The clock style's row of shortcuts (ohpanel_hmi.py home_clock_page): the shortcuts configured, in their order, at
   every second of its places, centred -- as many as the row has room for (its places + 1, halved). The places between
   them, and those left over, hidden with their touch areas (they overlap the ones shown). */
void Engine::draw_home_shortcuts(const cJSON *h) {
  const int places = clock_places_;
  std::vector<int> shortcuts;
  for (int i = 0; i < 12 && static_cast<int>(shortcuts.size()) < (places + 1) / 2; i++)
    if (slot(h, "shortcuts", i))
      shortcuts.push_back(i);
  clock_slots_.assign(places, -1);
  const int n = static_cast<int>(shortcuts.size()), first = places / 2 - (n - 1);
  for (int k = 0; k < n; k++)
    clock_slots_[first + 2 * k] = shortcuts[k];
  for (int p = 0; p < places; p++) {
    const std::string obj = "slot" + std::to_string(p + 1) + "_icon";
    const cJSON *s = clock_slots_[p] >= 0 ? slot(h, "shortcuts", clock_slots_[p]) : nullptr;
    vis("slot" + std::to_string(p + 1) + "_hs", s != nullptr);
    if (!s) {
      anims_.erase(obj);
      vis(obj, false);
      continue;
    }
    vis(obj, true);
    bind(s, [this, s, obj] {
      if (shown(s)) {
        show_icon(obj, s, color_of(s, "color", theme_.text));
      } else {
        anims_.erase(obj);
        txt(obj, "");
      }
    });
  }
}

/* A weather icon's name (the Meteocons' of weather-icons.json: "partly-cloudy-day-rain") as one of the icon font
   (Material Design's weather-*): the clock style's small icon, where home's picture has no room. */
std::string Engine::weather_glyph(const std::string &name) {
  const auto has = [&name](const char *part) { return name.find(part) != std::string::npos; };
  const bool night = has("night"), partly = has("partly");
  if (name.empty() || name == "not-available") return "weather-cloudy-alert";
  if (has("thunderstorms")) return has("rain") ? "weather-lightning-rainy" : "weather-lightning";
  if (has("tornado")) return "weather-tornado";
  if (has("hurricane")) return "weather-hurricane";
  if (has("hail")) return "weather-hail";
  if (has("sleet")) return "weather-snowy-rainy";
  if (has("snow")) return partly ? "weather-partly-snowy" : "weather-snowy";
  if (has("rain")) return partly ? "weather-partly-rainy" : "weather-pouring";
  if (has("drizzle")) return partly ? "weather-partly-rainy" : "weather-rainy";
  if (has("fog") || has("mist")) return "weather-fog";
  if (has("haze") || has("smoke")) return "weather-hazy";
  if (has("dust")) return "weather-dust";
  if (has("wind")) return "weather-windy";
  if (partly) return night ? "weather-night-partly-cloudy" : "weather-partly-cloudy";
  if (has("cloudy") || has("overcast")) return "weather-cloudy";
  if (has("clear")) return night ? "weather-night" : "weather-sunny";
  return "weather-cloudy";
}

}  // namespace appdisplay
