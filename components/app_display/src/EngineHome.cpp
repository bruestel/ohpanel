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

void Engine::draw_clock() {
  if (shown_ != "home" || now_ == 0)
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
  const bool pause = ampm && !refresh_paused_;
  if (pause)
    send("ref_stop");
  txt("clock", format_time(time_format, t, settings_.texts));
  vis("ampm", ampm);
  if (ampm) {
    sent_.erase("ampm.txt");   // drawn again even if unchanged
    txt("ampm", format_time("%p", t, settings_.texts));
  }
  if (pause)
    send("ref_star");
  txt("date", format_time(settings_.date_format, t, settings_.texts));  // dimmed by the page (th_dim)
}

void Engine::draw_relays() {
  const cJSON *home = member(config_, "home");
  for (int i = 0; i < 2; i++) {
    const cJSON *r = slot(home, "relays", i);
    const std::string side = "relay" + std::to_string(i + 1);
    if (!r) {
      vis(side + "_text", false);
      vis(side + "_bar", false);
      continue;
    }
    // the relay where there is one (it switches at once), else the item
    const bool is_on = member(r, "item") && !flag(r, "relay", true) ? on(r) : relays_[i];
    txt(side + "_text", str(r, "label"));
    // the bar under it (boards/nspanel/nextion/ohpanel_hmi.py home_page): the accent while on
    num(side + "_bar", "bco", is_on ? theme_.accent : kDarkGrey);
  }
}

void Engine::draw_home() {
  const cJSON *h = member(config_, "home");
  send("vis 255,1");  // the page hides everything while it loads
  hide_busy();  // all but that
  draw_clock();
  draw_wifi();

  // without an item in the configuration, the temperatures and the weather icon do not show
  // the value rows: an icon and a value each; the first may come as
  // indoor_temperature, its older name, with the house as its icon
  for (int i = 0; i < 2; i++) {
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
                                                    : str(v, "icon").c_str()),
                style.blink);
      std::string number, unit;
      number_and_unit(v, number, unit, &style);
      show_number(row + "_u", true, number, unit, style.color, style.blink);
    });
  }
  const cJSON *outdoor = member(h, "outdoor_temperature");
  if (!outdoor)
    show_number("outdoor_u", false);
  else
    bind(outdoor, [this, outdoor] {
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
  for (const char *key : {"outdoor_low", "outdoor_high"})
    if (const cJSON *r = member(h, key))
      bind(r, [this, h] { draw_range(h); });
  const cJSON *weather = member(h, "weather_icon");
  vis("weather_pic", weather != nullptr);
  vis("weather_hs", weather != nullptr);
  if (weather)
    bind(weather, [this, weather] {
      const int pic = weather_pic(weather, "weather_home");
      if (pic >= 0) num("weather_pic", "pic", pic);
    });

  for (int i = 0; i < 2; i++)
    if (const cJSON *r = slot(h, "relays", i))
      bind(r, [this] { draw_relays(); });
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
      const std::string show = str(s, "show", member(s, "item") ? "when_on" : "always");
      if (show == "always" || on(s)) {
        show_icon(obj, s, color_of(s, "color", theme_.text));
      } else {
        anims_.erase(obj);
        txt(obj, "");
      }
    });
  }

  // shortcuts: shortcut1_icon .. shortcut7_icon
  for (int i = 0; i < 7; i++) {
    const cJSON *s = slot(h, "shortcuts", i);
    const std::string obj = "shortcut" + std::to_string(i + 1) + "_icon";
    if (!s) {
      txt(obj, "");
      continue;
    }
    bind(s, [this, s, obj] {
      const std::string show = str(s, "show", member(s, "item") ? "when_on" : "always");
      if (show == "always" || on(s)) {
        show_icon(obj, s, color_of(s, "color", theme_.text));
      } else {
        anims_.erase(obj);
        txt(obj, "");
      }
    });
  }

  // info lines: infoN_tag (left), infoN_text (main), infoN_right (right)
  for (int line = 0; line < 2; line++) {
    const cJSON *l = slot(h, "info", line);
    const std::string n = "info" + std::to_string(line + 1);
    const char *fields[3] = {"left", "main", "right"};
    const std::string objs[3] = {n + "_tag", n + "_text", n + "_right"};
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
      const bool icon = f == 0 && member(ref, "icon"), bg = f == 0 && member(ref, "bg");
      const int fg = color_of(ref, "color", theme_.text);
      const std::string badge_pic = bg ? std::to_string(picture("badge" + std::to_string(line + 1),
                                                                nearest_palette(color_of(ref, "bg", 0))))
                                       : "";
      const bool coloured = member(ref, "color") != nullptr;
      bind(ref, [this, ref, variants, plain, ico, icon, bg, fg, coloured, badge_pic, f, badge_of] {
        const RuleStyle style = style_of(ref);
        const std::string value = !ref ? "" : icon ? glyph(ref) : value_of(ref, nullptr, &style);
        const std::string base = icon ? ico : plain;
        const std::string obj = bg && !value.empty() && badge_pic != "-1" ? badge_of(base) : base;
        for (const auto &v : variants)
          vis(v, v == obj);
        if (obj != base)
          num(obj, "pic", atoi(badge_pic.c_str()));
        // a colour of a matching value_style rule, or its own; else the page's: text, the right
        // field (a time, mostly) dimmed (th_*)
        if (style.color >= 0)
          num(obj, "pco", style.color);
        else if (coloured || obj != base || icon)
          num(obj, "pco", fg);
        else
          page_colour(obj, f == 2 ? theme_.dim : theme_.text);
        for (const auto &v : variants)
          if (v != obj)
            anims_.erase(v);
        show_text(obj, value, style.blink);
      });
    }
  }
  txt("top_icon", "");
  txt("top_value", "");
}



}  // namespace appdisplay
