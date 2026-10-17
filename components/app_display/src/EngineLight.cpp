/*
   The light page (Engine::draw_light): brightness, colour temperature, the colour wheel.
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

// -------------------------------------------------------- light and cover --
//
// boards/nspanel/nextion/ohpanel_hmi.py control_page(): the icon and the value on the left,
// a slider upright in the middle (the light: or the colour wheel), the column
// of tiles on the right -- the light's views, the cover's up / stop / down.


namespace {
constexpr int kKelvinMin = 2000, kKelvinMax = 6500;   // a colour temperature in K: its slider's ends by default
constexpr int64_t kColourAfterOnMs = 5000;            // a colour picked while off waits this long for the light
constexpr int64_t kWheelHoldMs = 1500;                 // after a touch on the wheel its item's echoes do not move the ring

/* "h,s,b" of a Color item; false for anything else. */
bool hsb_of(const std::string &state, double &h, double &s, double &b) {
  return std::sscanf(state.c_str(), "%lf,%lf,%lf", &h, &s, &b) == 3;
}

/* A colour at full brightness as RGB565. */
int hsv565(double h, double s) {
  h = std::fmod(std::fmod(h, 360) + 360, 360) / 60;
  s = std::clamp(s / 100, 0.0, 1.0);
  const double c = s, x = c * (1 - std::fabs(std::fmod(h, 2) - 1)), m = 1 - s;
  double r = 0, g = 0, b = 0;
  switch (static_cast<int>(h) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const auto ch = [m](double v, int bits) { return static_cast<int>(std::lround((v + m) * ((1 << bits) - 1))); };
  return ch(r, 5) << 11 | ch(g, 6) << 5 | ch(b, 5);
}

}  // namespace



/* How the colour temperature runs: in Kelvin (by default with an item in
   K), in Mired (1,000,000 / K: high is warm) or in percent (by default
   otherwise; 0 warm unless "reverse") -- "unit". Its ends ("min", "max") in
   that unit: 2000 .. 6500 K, 153 .. 500 mired, 0 .. 100 %. The slider runs
   warm (bottom) to cold (top) whatever the unit. */
Engine::ColourTemp Engine::colour_temp(const cJSON *page) const {
  ColourTemp t;
  const appoh::ItemState s = state_of(member(page, "color_temperature"));
  const std::string unit = str(page, "unit");
  t.unit = unit == "kelvin" || unit == "mired" || unit == "percent" ? unit : s.unit == "K" ? "kelvin" : "percent";
  t.with_unit = s.unit == "K";
  const double lo = t.unit == "kelvin" ? kKelvinMin : t.unit == "mired" ? 153 : 0;
  const double hi = t.unit == "kelvin" ? kKelvinMax : t.unit == "mired" ? 500 : 100;
  // the page's ends, else the item's in openHAB (its stateDescription: a lamp's minimum and maximum), else the
  // unit's usual ones
  appoh::Limits l;
  const bool known = cb_.limits && cb_.limits(str(member(page, "color_temperature"), "item"), l);
  t.min = num_of(page, "min", known && l.has_min ? l.min : lo);
  t.max = num_of(page, "max", known && l.has_max ? l.max : hi);
  // warm at the slider's bottom: the low end in K, the high end in mired; "reverse" turns it round
  t.warm_high = (t.unit == "mired") != flag(page, "reverse");
  return t;
}

int Engine::temp_slider(const cJSON *page) const {
  const appoh::ItemState s = state_of(member(page, "color_temperature"));
  if (!s.has_numeric)
    return 0;
  const ColourTemp t = colour_temp(page);
  const double share = (s.numeric - t.min) / std::max(1e-9, t.max - t.min);
  const int v = std::clamp(static_cast<int>(std::lround(share * 100)), 0, 100);
  return t.warm_high ? 100 - v : v;
}

/* What the colour temperature at the slider's `value` is, as shown and as
   the command for its item. */
std::pair<std::string, std::string> Engine::temp_at(const cJSON *page, int value) const {
  const ColourTemp t = colour_temp(page);
  const double share = (t.warm_high ? 100 - value : value) / 100.0;
  const long v = std::lround(t.min + (t.max - t.min) * share);
  if (t.unit == "kelvin")
    return {std::to_string(v) + " K", std::to_string(v) + (t.with_unit ? " K" : "")};
  if (t.unit == "mired")
    return {std::to_string(v > 0 ? std::lround(1e6 / v / 10) * 10 : 0) + " K", std::to_string(v)};   // as Kelvin
  return {std::to_string(v) + " %", std::to_string(v)};
}

void Engine::draw_light(const cJSON *page) {
  txt("title", str(page, "title"));
  const bool views[3] = {true, member(page, "color_temperature") != nullptr, member(page, "color") != nullptr};
  if (light_view_ < 0 || light_view_ > 2 || !views[light_view_])
    light_view_ = 0;
  // the tiles only with more than the brightness to choose from: one for each view the light has
  const bool tiles = views[1] || views[2];
  const char *const icons[3] = {"brightness-6", "thermometer", "palette"};
  for (int k = 0; k < 3; k++) {
    const std::string n = "side" + std::to_string(k + 1);
    const bool shown = tiles && views[k], active = k == light_view_;
    if (shown) {
      if (const int pic = picture(n, active ? "on" : "off"); pic >= 0)
        num(n + "_pic", "pic", pic);
      for (const std::string &part : {n + "_icon", n + "_text"})
        num(part, "bco", active ? kWhite : kDarkGrey);
      num(n + "_icon", "pco", active ? theme_.accent : theme_.dim);
      num(n + "_text", "pco", active ? kDarkGrey : kLightGrey);
      txt(n + "_icon", icon_glyph(icons[k]));
      txt(n + "_text", settings_.texts.light_views[k]);
    }
    for (const std::string &part : {n + "_pic", n + "_icon", n + "_text", n + "_hs"})
      vis(part, shown);
  }
  // one view at a time: the brightness's slider, the colour temperature's, the colour wheel
  if (light_view_ == 2)
    if (const int pic = picture("wheel", "disc"); pic >= 0)
      num("wheel_pic", "pic", pic);
  vis("slider", light_view_ == 0);
  vis("temp_slider", light_view_ == 1);
  for (const char *obj : {"wheel_pic", "wheel_hs"})
    vis(obj, light_view_ == 2);
  vis("icon", true);
  vis("value", light_view_ != 2);
  // the on / off tile -- in the colour view the colour chosen in its place (draw_wheel_mark)
  for (const char *part : {"power_pic", "power_icon", "power_text", "power_hs"})
    vis(part, light_view_ != 2);
  for (const char *key : {"brightness", "color_temperature", "color", "switch"})
    bind(member(page, key), [this, page] { light_refresh(page); });
}

bool Engine::light_on(const cJSON *page) const {
  if (member(page, "switch"))
    return on(member(page, "switch"));
  const appoh::ItemState bri = state_of(member(page, "brightness"));
  return bri.has_numeric && bri.numeric > 0;
}

void Engine::light_refresh(const cJSON *page) {
  const appoh::ItemState bri = state_of(member(page, "brightness"));
  double h = 0, s = 0, b = 0;
  const bool colour = hsb_of(state_of(member(page, "color")).state, h, s, b);
  const int level = bri.has_numeric ? static_cast<int>(std::lround(bri.numeric)) : colour ? static_cast<int>(b) : 0;
  const bool lit = light_on(page);
  // a colour picked while it was off: sent now that it is on, at the brightness it came back with
  if (colour_after_on_ && colour_after_on_->page == str(page, "id") && lit) {
    const int at = colour && b > 0 ? static_cast<int>(std::lround(b)) : level;
    if (at > 0) {
      const std::string item = str(member(page, "color"), "item");
      const std::string hsb = std::to_string(colour_after_on_->hue) + "," + std::to_string(colour_after_on_->sat) + "," +
                              std::to_string(at);
      colour_after_on_.reset();
      if (!item.empty() && cb_.command) {
        cb_.command(item, hsb);
        if (cb_.expect)
          cb_.expect(item, hsb);
      }
    }
  }
  // the icon lit in the accent while the light is on; the on / off tile (bottom left) white then; the
  // brightness slider's fill grey while off
  txt("icon", icon_glyph(str(page, "icon", "lightbulb").c_str()));
  num("icon", "pco", lit ? theme_.accent : theme_.dim);
  if (const int pic = picture("power", lit ? "on" : "off"); pic >= 0)
    num("power_pic", "pic", pic);
  for (const char *part : {"power_icon", "power_text"})
    num(part, "bco", lit ? kWhite : kDarkGrey);
  num("power_icon", "pco", lit ? theme_.accent : theme_.dim);
  num("power_text", "pco", lit ? kDarkGrey : kLightGrey);
  txt("power_icon", icon_glyph("power"));
  txt("power_text", lit ? settings_.texts.on : settings_.texts.off);
  num("slider", "bco1", lit ? theme_.accent : kSliderOff);
  if (slider_held() && light_view_ != 2) {
    // the finger on the slider (or just off it): it stays where the finger put it (slider_moved)
  } else if (light_view_ == 0) {
    slider_val("slider", std::clamp(level, 0, 100));
    const bool known = (bri.has_numeric && !bri.undefined()) || colour;   // NULL, UNDEF, not known yet: "-"
    txt("value", known ? std::to_string(level) + " %" : settings_.texts.undefined);
  } else if (light_view_ == 1) {
    const int slider = temp_slider(page);
    slider_val("temp_slider", slider);
    txt("value", has_state(member(page, "color_temperature")) ? temp_at(page, slider).first : "");
  } else if (colour && now_ms_ - wheel_touched_ms_ >= kWheelHoldMs) {
    // not right after a touch: the lamp echoes its old colour first, the ring would jump back and forth
    draw_wheel_mark(h, s);
  }
}

/* The colour chosen on the wheel: a ring where it is (the wheel drawn again
   first, wiping the last one), and as a disc where the on / off tile is in
   the other views. */
void Engine::draw_wheel_mark(double h, double s) {
  const auto pg = boxes_.find("light");
  if (pg == boxes_.end() || !pg->second.count("wheel_pic") || !pg->second.count("swatch") || note_drawn_)
    return;
  const std::string key = std::to_string(std::lround(h)) + "," + std::to_string(std::lround(s));
  std::string &drawn = drawn_["wheel"];
  if (drawn == key)
    return;
  const Box &w = pg->second.at("wheel_pic"), &sw = pg->second.at("swatch");
  // the old ring wiped with the piece of the wheel's picture under it (xpic: drawn at once, unlike ref,
  // which the display may get round to only after the new ring -- wiping that too)
  const int pic = picture("wheel", "disc");
  if (!drawn.empty() && wheel_ring_x_ >= 0 && pic >= 0) {
    const int bx = wheel_ring_x_ - kRingSize / 2, by = wheel_ring_y_ - kRingSize / 2;
    send("xpic " + std::to_string(bx) + "," + std::to_string(by) + "," + std::to_string(kRingSize) + "," +
         std::to_string(kRingSize) + "," + std::to_string(bx - w.x) + "," + std::to_string(by - w.y) + "," +
         std::to_string(pic));
  }
  drawn = key;
  // the ring inside the wheel's picture, also for the strongest colours at its edge
  const double reach = w.w / 2.0 - kRingSize / 2.0 - 1;
  const double r = reach * std::clamp(s / 100, 0.0, 1.0), a = h * M_PI / 180;
  const int x = static_cast<int>(std::lround(w.x + w.w / 2.0 + std::cos(a) * r));
  const int y = static_cast<int>(std::lround(w.y + w.h / 2.0 - std::sin(a) * r));
  wheel_ring_x_ = x;
  wheel_ring_y_ = y;
  // a ring dark outside and white inside: seen on the wheel's pale middle as on its strong edge
  for (int k = 11; k >= 6; k--)
    send("cir " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(k) + "," +
         std::to_string(k >= 10 || k == 6 ? kSymbol : kWhite));
  const int cx = sw.x + sw.w / 2, cy = sw.y + sw.h / 2, sr = sw.w / 2;
  disc(cx, cy, sr, kWhite);
  disc(cx, cy, sr - 2, hsv565(h, s));
}

/* A slider of the light page moved ("...s": every kSlideMs at most) or was
   let go ("...u"), or the wheel was touched ("cwhl": x * 1000 + y). */
void Engine::light_value(const std::string &name, int value) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || !cb_.command || name.size() != 4)
    return;
  if (name == "cwhl") {
    const auto pg = boxes_.find("light");
    const std::string item = str(member(page, "color"), "item");
    if (pg == boxes_.end() || !pg->second.count("wheel_pic") || item.empty())
      return;
    const Box &w = pg->second.at("wheel_pic");
    const double dx = value / 1000 - (w.x + w.w / 2.0), dy = value % 1000 - (w.y + w.h / 2.0);
    const double r = std::sqrt(dx * dx + dy * dy) / (w.w / 2.0);
    if (r > 1.05)
      return;   // beside the disc
    const long hue = std::lround(std::fmod(std::atan2(-dy, dx) * 180 / M_PI + 360, 360)) % 360;
    const long sat = std::lround(std::min(r, 1.0) * 100);
    double h = 0, s = 0, b = 0;
    wheel_touched_ms_ = now_ms_;
    draw_wheel_mark(hue, sat);
    if (!light_on(page) || !hsb_of(state_of(member(page, "color")).state, h, s, b) || b <= 0) {
      // off: switched on first; the colour follows once it is on, at the brightness it comes back with
      if (!colour_after_on_ || colour_after_on_->page != str(page, "id"))
        light_power(page);
      colour_after_on_ = ColourAfterOn{str(page, "id"), static_cast<int>(hue), static_cast<int>(sat),
                                       now_ms_ + kColourAfterOnMs};
      return;
    }
    const std::string hsb = std::to_string(hue) + "," + std::to_string(sat) + "," + std::to_string(std::lround(b));
    wheel_pending_ = {item, hsb};
    send_wheel(false);
    return;
  }
  const bool done = name.back() == 'u';
  slider_moved(name.rfind("ctp", 0) == 0 ? "temp_slider" : "slider", std::clamp(value, 0, 100));
  // while it moves too (every kSlideMs at most), unless the page says "live": false -- then only shown
  if (!done && (!flag(page, "live", true) || now_ms_ - light_sent_ms_ < kSlideMs)) {
    if (!done && name.rfind("csl", 0) == 0)
      txt("value", std::to_string(std::clamp(value, 0, 100)) + " %");
    return;
  }
  light_sent_ms_ = now_ms_;
  value = std::clamp(value, 0, 100);
  if (name.rfind("csl", 0) == 0) {
    txt("value", std::to_string(value) + " %");
    if (const std::string item = str(member(page, "brightness"), "item"); !item.empty())
      cb_.command(item, std::to_string(value));
  } else if (name.rfind("ctp", 0) == 0) {
    const std::string item = str(member(page, "color_temperature"), "item");
    if (item.empty())
      return;
    const auto [shown, command] = temp_at(page, value);
    txt("value", shown);
    cb_.command(item, command);
  }
}

/* The colour the wheel was last touched at to its item -- every kSlideMs at
   most while the finger moves; `now`: the last one, whenever (idle()). */
void Engine::send_wheel(bool now) {
  if (!wheel_pending_ || !cb_.command || (!now && now_ms_ - wheel_sent_ms_ < kSlideMs))
    return;
  wheel_sent_ms_ = now_ms_;
  cb_.command(wheel_pending_->first, wheel_pending_->second);
  if (cb_.expect)
    cb_.expect(wheel_pending_->first, wheel_pending_->second);
  wheel_pending_.reset();
}

/* The light switched from its tile: ON / OFF to its "switch", else to the brightness item -- openHAB and
   the lamp keep the rest. */
void Engine::light_power(const cJSON *page) {
  const std::string item = str(member(page, member(page, "switch") ? "switch" : "brightness"), "item");
  if (!item.empty() && cb_.command)
    cb_.command(item, light_on(page) ? "OFF" : "ON");
}

/* The finger moved the light's or the cover's slider to `value`: that is
   what the display shows now (sent_ knows it so). */
void Engine::slider_moved(const char *slider, int value) {
  slider_moved_ms_ = now_ms_;
  slider_resync_ = true;
  slider_left_ = value;
  sent_[std::string(slider) + ".val"] = std::to_string(value);
}

/* The slider to what the item says -- left where the finger put it while
   that is within a step of it (a lamp reports 39 for the 40 it was sent):
   moving the handle draws the whole slider anew. */
void Engine::slider_val(const char *slider, int value) {
  if (slider_left_ >= 0 && std::abs(value - slider_left_) <= 1)
    return;
  slider_left_ = -1;
  num(slider, "val", value);
}


}  // namespace appdisplay
