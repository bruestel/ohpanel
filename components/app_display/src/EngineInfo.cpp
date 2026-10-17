/*
   The weather page, the info and network pages, the QR page and the settings page (Engine::draw_weather,
   draw_system, draw_qr, draw_settings).
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

std::vector<const cJSON *> Engine::days_of(const cJSON *page) const {
  std::vector<const cJSON *> out;
  const cJSON *days = member(page, "days");
  for (const cJSON *d = cJSON_IsArray(days) ? days->child : nullptr; d; d = d->next)
    if (cJSON_IsObject(d))
      out.push_back(d);
  if (out.empty())
    out.push_back(page);
  return out;
}

/* The days of the forecast (boards/nspanel/nextion/ohpanel_hmi.py weather_page), a sheet
   each: the day -- its title, else today / tomorrow / its weekday -- and its
   date as the title, the icon, the high large and the low under it ("13,4°",
   the scale left out as on home), the condition in words, six cards of an
   extra: an icon, the value, a small label. Everything starts hidden: what the day has is shown. */
void Engine::draw_weather(const cJSON *page) {
  const std::vector<const cJSON *> days = days_of(page);
  const int n = static_cast<int>(days.size());
  if (sheet_ >= n)
    sheet_ = 0;
  const cJSON *d = days[sheet_];
  const cJSON *off = member(d, "day");
  const int ahead = cJSON_IsNumber(off) ? off->valueint : 0;
  const Texts &t = settings_.texts;
  std::string title = str(d, "title");
  struct tm when = {};
  const time_t day = now_ + static_cast<time_t>(ahead) * 86400;
  if (now_)
    localtime_r(&day, &when);
  if (title.empty())
    title = ahead == 0 ? t.today : ahead == 1 ? t.tomorrow : now_ ? t.weekdays[when.tm_wday] : "";
  if (now_)
    title += (title.empty() ? "" : " · ") + format_time(settings_.date_format, when, t);
  txt("title", title);
  // the days as dots at the bottom (as an entity page's sheets), the one shown full
  std::string dots;
  for (int k = 0; k < n && n > 1; k++)
    dots += (k ? " " : "") + icon_glyph(k == sheet_ ? "circle-medium" : "circle-small");
  txt("dots", dots);
  vis("dots", n > 1);

  const cJSON *icon = member(d, "icon");
  bind(icon, [this, icon] {
    const int pic = weather_pic(icon, "weather_page");
    if (pic >= 0) {   // shown once it has the icon of the background in use
      num("weather_pic", "pic", pic);
      vis("weather_pic", true);
    }
  });
  const auto degrees = [this](const cJSON *ref) {
    std::string number, unit;
    number_and_unit(ref, number, unit);
    return number.empty() ? number : number + degrees_only(unit);
  };
  // the temperatures: the high large and the low under it -- or, with the condition in words, the high smaller with
  // the low right after it (at the place its width needs) and the condition large under them
  const cJSON *mx = member(d, "max"), *mn = member(d, "min"), *cond = member(d, "condition");
  const auto temps = [this, mx, mn, cond, degrees] {
    const std::string high = mx ? degrees(mx) : "", low = mn ? degrees(mn) : "";
    const std::string words = cond ? value_of(cond) : "";
    const bool line = !words.empty();
    const int n = std::max(1, weather_line_.n);
    int k = 0;   // the low's place on the line
    if (line && mn) {
      const int need = weather_line_.high_x + (high.empty() ? 0 : text_width(weather_line_.font, high) + 10);
      k = std::clamp((need - weather_line_.x0 + weather_line_.step - 1) / weather_line_.step + 1, 1, n);
    }
    // the fields going first (hiding repaints the background over them), then those that show
    const auto at = [](const char *part, int j) { return std::string(part) + std::to_string(j); };
    for (int j = 1; j <= n; j++)
      if (j != k) {
        vis(at("low_arrow", j), false);
        vis(at("low", j), false);
      }
    if (line) {
      for (const char *o : {"high", "low_arrow", "low"})
        vis(o, false);
      txt("high_line", high);
      txt("condition", words);
      vis("high_line", mx != nullptr);
      vis("condition", true);
      if (k) {
        txt(at("low", k), low);
        vis(at("low_arrow", k), true);
        vis(at("low", k), true);
      }
    } else {
      for (const char *o : {"high_line", "condition"})
        vis(o, false);
      txt("high", high);
      txt("low", low);
      vis("high", mx != nullptr);
      vis("low_arrow", mn != nullptr);
      vis("low", mn != nullptr);
    }
  };
  for (const cJSON *ref : {mx, mn, cond})
    if (ref)
      bind(ref, temps);
  if (!mx && !mn && !cond)
    temps();

  for (int k = 0; k < kWeatherChips; k++) {
    const cJSON *e = slot(d, "extras", k);
    const std::string c = "chip" + std::to_string(k + 1);
    if (!e) {
      for (const char *part : {"_pic", "_icon", "_value", "_text"})
        vis(c + part, false);
      continue;
    }
    if (const int pic = picture("wchip" + std::to_string(k + 1), "box"); pic >= 0)
      num(c + "_pic", "pic", pic);
    txt(c + "_icon", glyph(e));
    num(c + "_icon", "pco", color_of(e, "color", theme_.accent));
    txt(c + "_text", str(e, "label"));
    for (const char *part : {"_pic", "_icon", "_value", "_text"})
      vis(c + part, true);
    bind(e, [this, e, c] { txt(c + "_value", value_of(e)); });
  }
}

namespace {
std::string kb(size_t bytes) { return std::to_string(bytes / 1024) + " KB"; }

std::string uptime(int64_t s) {
  char buf[24];
  const int d = static_cast<int>(s / 86400), h = static_cast<int>(s / 3600 % 24), m = static_cast<int>(s / 60 % 60);
  if (d > 0)
    snprintf(buf, sizeof(buf), "%d d %02d:%02d", d, h, m);
  else
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, static_cast<int>(s % 60));
  return buf;
}
}  // namespace

/* The panel's own values (boards/nspanel/nextion/ohpanel_hmi.py system_page): eight
   cards, an icon, a label and the value each -- a value too long for its line
   in two smaller lines under a higher label. Redrawn on every tick while
   shown; unchanged ones are not resent. */
void Engine::draw_system(const cJSON *page) {
  const SystemInfo i = cb_.system ? cb_.system() : SystemInfo{};
  const Texts &t = settings_.texts;
  // the IPv6 address the card shows: a global one, else a unique local one
  std::string v6;
  for (const char *kind : {"global", "unique-local"})
    for (const auto &[k, a] : i.ipv6)
      if (v6.empty() && k == kind)
        v6 = a;
  /* An IPv6 address too long for `field`: its start and its last group, "…" between them
     ("2003:d6:470e:4000:…:d4e0"), groups dropped from the start's end until it fits. */
  const auto shortened_v6 = [this](const std::string &field, const std::string &a) {
    if (fits(shown_, field, a))
      return a;
    const size_t last = a.rfind(':');
    std::string head = a.substr(0, last);
    while (!head.empty()) {
      const std::string s = head + ":…" + a.substr(last);
      if (fits(shown_, field, s))
        return s;
      const size_t cut = head.rfind(':');
      head = cut == std::string::npos ? "" : head.substr(0, cut);
    }
    return a;
  };
  const std::string none = t.undefined;
  // openHAB: its host as the value, the protocol and the port with the label
  std::string oh_host = i.openhab, oh_label = t.system[7];
  if (!i.openhab.empty()) {
    std::string port;
    if (const size_t colon = i.openhab.rfind(':'); colon != std::string::npos && i.openhab.find(']', colon) == std::string::npos) {
      oh_host = i.openhab.substr(0, colon);
      port = i.openhab.substr(colon + 1);
    }
    oh_label += std::string(" · ") + (i.openhab_https ? "https" : "http") + (port.empty() ? "" : " " + port);
  }
  const char *signal = !i.rssi       ? "wifi-strength-off-outline"
                       : i.rssi > -55 ? "wifi-strength-4"
                       : i.rssi > -67 ? "wifi-strength-3"
                       : i.rssi > -78 ? "wifi-strength-2"
                                      : "wifi-strength-1";
  struct Card {
    const char *icon;
    std::string label, value;
    const char *badge;
  };
  const Card cards[8] = {
      {"information-outline", t.system[0], i.version.empty() ? none : i.version, ""},
      {"monitor", t.system[1],
       i.display_outdated ? t.design_outdated : i.display_design.empty() ? none : i.display_design, ""},
      {"timer-outline", t.system[2], uptime(i.uptime_s), ""},
      {"memory", t.system[3], kb(i.heap_free) + " " + t.free, ""},
      {"chip", t.system[4],
       i.psram_total ? format_number(i.psram_free / 1048576.0, 1, false, settings_.decimal) + " / " +
                           format_number(i.psram_total / 1048576.0, 1, false, settings_.decimal) + " MB"
                     : none,
       ""},
      {"wifi", t.system[5], i.ssid.empty() ? none : i.ssid, signal},
      {"ip-network", t.system[6], i.ip.empty() ? (v6.empty() ? none : v6) : i.ip, ""},
      {"ohpanel-openhab", oh_label, oh_host.empty() ? none : oh_host, ""},
  };
  txt("title", str(page, "title", t.system_title));
  for (int k = 0; k < 8; k++) {
    const Card &c = cards[k];
    const std::string n = "card" + std::to_string(k + 1);
    if (const int pic = picture(n, "box"); pic >= 0)   // the card on the background in use
      num(n + "_pic", "pic", pic);
    txt(n + "_icon", icon_glyph(c.icon));
    num(n + "_icon", "pco", theme_.accent);
    // the IP card with an IPv6 address: IPv4 above it, IPv6 below (shortened in the middle); a tap opens the
    // network page, the label says so
    const bool ip_card = k == 6 && !v6.empty();
    const bool one = !ip_card && fits(shown_, n + "_value", c.value);
    if (one) {
      txt(n + "_text", c.label);
      txt(n + "_value", c.value);
    } else if (ip_card) {
      txt(n + "_top", c.label + " ›");
      if (i.ip.empty()) {   // IPv6 only: the address on two lines
        const auto [first, second] = two_lines(shown_, n + "_line1", v6);
        txt(n + "_line1", first);
        txt(n + "_line2", second);
      } else {
        txt(n + "_line1", i.ip);
        txt(n + "_line2", shortened_v6(n + "_line2", v6));
      }
    } else {   // too long for the line: in two, smaller, under the label set higher
      const auto [first, second] = two_lines(shown_, n + "_line1", c.value);
      txt(n + "_top", c.label);
      txt(n + "_line1", first);
      txt(n + "_line2", second);
    }
    txt(n + "_badge", *c.badge ? icon_glyph(c.badge) : "");
    for (const std::string &part : {n + "_pic", n + "_icon"})
      vis(part, true);
    for (const char *part : {"_text", "_value"})
      vis(n + part, one);
    for (const char *part : {"_top", "_line1", "_line2"})
      vis(n + part, !one);
    vis(n + "_badge", *c.badge != 0);   // last: on top of the lines
  }
  vis("card7_hs", true);   // the IP card opens the network page
}

/* The network page: label and value per row, as many rows as there is to say (nine at most), the rest
   hidden. Over the info page: closing it (or the title's back arrow) comes back to that. */
void Engine::draw_network_page() {
  const SystemInfo i = cb_.system ? cb_.system() : SystemInfo{};
  const Texts &t = settings_.texts;
  txt("title", "‹ " + t.network_title);
  std::vector<std::pair<std::string, std::string>> rows;
  if (!i.ip.empty())
    rows.emplace_back(t.network[0], i.ip + (i.prefix ? " / " + std::to_string(i.prefix) : ""));
  if (!i.gateway.empty())
    rows.emplace_back(t.network[1], i.gateway);
  for (const auto &[label, kind] : {std::pair{t.network[2], "global"}, std::pair{t.network[3], "unique-local"},
                                    std::pair{t.network[4], "link-local"}})
    for (const auto &[k, a] : i.ipv6)
      if (k == kind)
        rows.emplace_back(label, a);
  if (i.gateway.empty() && !i.gateway6.empty())   // the gateway of an IPv6-only network
    rows.emplace_back(t.network[5], i.gateway6);
  for (const std::string &d : i.dns)
    rows.emplace_back(t.network[6], d);
  if (!i.time_server.empty())
    rows.emplace_back(t.network[7], i.time_server);
  if (!i.name.empty())
    rows.emplace_back(t.network[8], i.name);
  if (rows.size() > 9) {   // the name and the time server stay; addresses are dropped from the middle
    rows.erase(rows.begin() + 9 - 2, rows.end() - 2);
  }
  for (size_t r = 0; r < 9; r++) {
    const std::string n = "row" + std::to_string(r + 1);
    const bool on = r < rows.size();
    if (on) {
      txt(n + "_text", rows[r].first);
      txt(n + "_value", rows[r].second);
    }
    vis(n + "_text", on);
    vis(n + "_value", on);
  }
}

namespace {
/* A Wi-Fi login's network and password (WIFI:T:WPA;S:name;P:secret;;, "\"
   escaping), for the caption's {ssid} and {password}; empty for other texts. */
std::pair<std::string, std::string> wifi_login(const std::string &text) {
  if (text.rfind("WIFI:", 0) != 0)
    return {};
  std::string key, value, ssid, password;
  bool in_key = true;
  for (size_t i = 5; i < text.size(); i++) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      (in_key ? key : value) += text[++i];
    } else if (in_key && c == ':') {
      in_key = false;
    } else if (c == ';') {
      if (key == "S") ssid = value;
      else if (key == "P") password = value;
      key.clear(), value.clear(), in_key = true;
    } else {
      (in_key ? key : value) += c;
    }
  }
  return {ssid, password};
}

std::string replace_all(std::string s, const std::string &what, const std::string &with) {
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
    s.replace(at, what.size(), with);
  return s;
}
}  // namespace

/* The QR page (boards/nspanel/nextion/ohpanel_hmi.py qr_page): the title, the caption in
   up to two lines (a text, or a list) -- {ssid} and {password} in it from a
   Wi-Fi login -- and the code, drawn by the firmware (draw_qr_code). */
void Engine::draw_qr(const cJSON *page) {
  txt("title", str(page, "title"));
  const auto [ssid, password] = wifi_login(str(page, "text"));
  const cJSON *caption = member(page, "caption");
  for (int k = 0; k < 2; k++) {
    const cJSON *line = cJSON_IsArray(caption) ? cJSON_GetArrayItem(caption, k) : k == 0 ? caption : nullptr;
    const std::string text = cJSON_IsString(line) ? line->valuestring : "";
    txt("caption" + std::to_string(k + 1), replace_all(replace_all(text, "{ssid}", ssid), "{password}", password));
  }
  draw_qr_code(page);
}

/* The QR page's code: its settings (style, colour -- the theme's accent
   unless one is given -- error correction) for draw_code. */
void Engine::draw_qr_code(const cJSON *page) {
  draw_code("qr_area", str(page, "text"), str(page, "ecc", "medium"), str(page, "style") == "dots",
            color_of(page, "color", theme_.accent), 6);
}

/* A QR code in an area ("boxes" of the manifest): a white card just around
   it (`pad` px), rounded a little, and the modules as filled rectangles, as
   large as fit -- the display's own QR object drew it column by column,
   visibly; these are a few hundred fills, from the middle outwards. `dots`:
   the modules apart (the three finder squares stay whole, scanners look for
   them; only from 4 px a module). Drawn again only when it changes
   (drawn_, emptied with each page) -- and then on the page loaded
   anew: what was drawn before is gone that way. */
void Engine::draw_code(const std::string &field, const std::string &text, const std::string &ecc, bool dots,
                       int colour, int pad) {
  constexpr int kRadius = 6;       // the card's corners
  constexpr int kMaxVersion = 10;  // 57 modules: 3 px each at the least
  const auto pg = boxes_.find(shown_);
  if (pg == boxes_.end() || !pg->second.count(field) || note_drawn_)
    return;
  const Box &area = pg->second.at(field);
  const std::string key = std::to_string(std::hash<std::string>{}(text)) + "|" + ecc + "|" + (dots ? "d" : "s") +
                          "|" + std::to_string(colour);
  std::string &drawn = drawn_[field];
  if (drawn == key)
    return;
  if (!drawn.empty()) {   // another code over the one shown: the page anew, then this one on it
    show_physical(shown_);
    return;
  }
  drawn = key;
  const qrcodegen_Ecc level = ecc == "low"        ? qrcodegen_Ecc_LOW
                              : ecc == "quartile" ? qrcodegen_Ecc_QUARTILE
                              : ecc == "high"     ? qrcodegen_Ecc_HIGH
                                                  : qrcodegen_Ecc_MEDIUM;
  std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)), tmp(qr.size());
  // the level asked for, raised where it costs nothing; lowered when the text does not fit otherwise
  bool ok = !text.empty() && qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), level, qrcodegen_VERSION_MIN,
                                                  kMaxVersion, qrcodegen_Mask_AUTO, true);
  if (!ok && !text.empty())
    ok = qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              kMaxVersion, qrcodegen_Mask_AUTO, false);
  if (!ok)
    return;   // nothing, or too long: the area stays empty
  const int n = qrcodegen_getSize(qr.data());
  const int m = std::max(1, (std::min(area.w, area.h) - 2 * pad) / n);
  const int card = n * m + 2 * pad;
  const int cx = area.x + (area.w - card) / 2, cy = area.y + (area.h - card) / 2;
  const int x0 = cx + pad, y0 = cy + pad;
  // the card: its rows shortened at the corners (a quarter circle), the middle at once
  fill(cx, cy + kRadius, card, card - 2 * kRadius, kWhite);
  for (int i = 0; i < kRadius; i++) {
    const double dy = kRadius - i - 0.5;
    const int inset = kRadius - static_cast<int>(std::lround(std::sqrt(kRadius * kRadius - dy * dy)));
    fill(cx + inset, cy + i, card - 2 * inset, 1, kWhite);
    fill(cx + inset, cy + card - 1 - i, card - 2 * inset, 1, kWhite);
  }
  const auto dark = [&](int x, int y) { return qrcodegen_getModule(qr.data(), x, y); };
  const auto finder = [n](int x, int y) { return ((x < 7 || x >= n - 7) && y < 7) || (x < 7 && y >= n - 7); };
  const bool apart = dots && m >= 4;
  struct Rect {
    int x, y, w, h;
  };
  std::vector<Rect> rects;
  std::map<std::pair<int, int>, size_t> open;   // (x, width) -> the rectangle ending in the row above
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n;) {
      if (!dark(x, y) || (apart && !finder(x, y))) {
        x++;
        continue;
      }
      int end = x;
      while (end < n && dark(end, y) && !(apart && !finder(end, y)))
        end++;
      const auto it = open.find({x, end - x});
      if (it != open.end() && rects[it->second].y + rects[it->second].h == y) {
        rects[it->second].h++;
      } else {
        open[{x, end - x}] = rects.size();
        rects.push_back({x, y, end - x, 1});
      }
      x = end;
    }
  }
  if (apart)   // the other modules apart: one each, a pixel less -- a gap between
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        if (dark(x, y) && !finder(x, y))
          rects.push_back({x, y, 1, 1});
  // from the middle outwards: the code grows from its centre instead of being wiped on from the top
  const auto distance = [n](const Rect &r) {
    const int dx = 2 * r.x + r.w - n, dy = 2 * r.y + r.h - n;   // twice the offset of its centre
    return dx * dx + dy * dy;
  };
  std::stable_sort(rects.begin(), rects.end(), [&](const Rect &a, const Rect &b) { return distance(a) < distance(b); });
  const int gap = apart ? 1 : 0;
  for (const Rect &r : rects) {
    const bool single = apart && r.w == 1 && r.h == 1 && !finder(r.x, r.y);
    fill(x0 + r.x * m, y0 + r.y * m, r.w * m - (single ? gap : 0), r.h * m - (single ? gap : 0), colour);
  }
}

/* Where the page sits in the swipe row, and for long lists which sheet. */

// --------------------------------------------------------------- settings --

namespace {
// the idle-after slider's steps, s
constexpr int kIdleSteps[] = {0, 15, 30, 60, 120, 300, 600};
constexpr int kIdleStepCount = sizeof(kIdleSteps) / sizeof(kIdleSteps[0]);

int idle_step(int seconds) {
  int best = 0;
  for (int k = 1; k < kIdleStepCount; k++)
    if (std::abs(kIdleSteps[k] - seconds) < std::abs(kIdleSteps[best] - seconds))
      best = k;
  return best;
}

std::string idle_text(int seconds, const Texts &t) {
  if (seconds <= 0) return t.never;
  if (seconds % 60 == 0) return std::to_string(seconds / 60) + " min";
  return std::to_string(seconds) + " s";
}
}  // namespace

/* The settings page: brightness in use and when idle (0 %: off), idle after
   (steps), restart. What is set here counts as set in the web interface:
   kept in the configuration, sent to a linked item. */
void Engine::draw_settings(const cJSON *page) {
  const Texts &t = settings_.texts;
  txt("title", str(page, "title", t.settings_title));
  restart_armed_ = 0;
  txt("restart", t.restart);
  // the button's tile, composed onto the background in use (compose.py "restart")
  if (const int off = picture("restart", "off"), on = picture("restart", "on"); off >= 0 && on >= 0) {
    num("restart", "pic", off);
    num("restart", "pic2", on);
  }
  vis("restart", true);   // hidden while the page loads: its design's tiles are the default background's
  const int values[3] = {screen_.brightness, screen_.idle_brightness, screen_.idle_after};
  for (int k = 0; k < 3; k++) {
    const std::string n = "set" + std::to_string(k + 1);
    txt(n + "_text", t.settings[k]);
    if (k == 2) {
      num(n + "_slider", "maxval", kIdleStepCount - 1);
      num(n + "_slider", "val", idle_step(values[k]));
      txt(n + "_value", idle_text(values[k], t));
    } else {
      num(n + "_slider", "minval", k == 0 ? 1 : 0);
      num(n + "_slider", "val", values[k]);
      txt(n + "_value", k == 1 && values[k] == 0 ? t.off : std::to_string(values[k]) + " %");
    }
  }
}

void Engine::settings_value(int slider, int value) {
  const Texts &t = settings_.texts;
  struct Key { int &value, &stored; const std::string &item; const char *name; };
  const Key keys[3] = {{screen_.brightness, settings_.screen.brightness, screen_.brightness_item, "brightness"},
                       {screen_.idle_brightness, settings_.screen.idle_brightness, screen_.idle_brightness_item,
                        "idle_brightness"},
                       {screen_.idle_after, settings_.screen.idle_after, screen_.idle_after_item, "idle_after"}};
  const Key &k = keys[slider];
  int v = slider == 2 ? kIdleSteps[std::clamp(value, 0, kIdleStepCount - 1)] : std::clamp(value, slider == 0 ? 1 : 0, 100);
  const std::string n = "set" + std::to_string(slider + 1);
  txt(n + "_value", slider == 2 ? idle_text(v, t) : slider == 1 && v == 0 ? t.off : std::to_string(v) + " %");
  if (v == k.value)
    return;
  k.value = k.stored = v;
  remember_[k.name] = v;        // kept once it settles ...
  if (!k.item.empty())
    command_[k.name] = v;       // ... and sent to its item then
  remember_at_ = now_ms_;
  if (slider == 0)
    set_level(v);               // the brightness shows while it moves
}



}  // namespace appdisplay
