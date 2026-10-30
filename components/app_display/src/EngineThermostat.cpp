/*
   The thermostat page (Engine::draw_thermostat): the ring, the target, minus and plus, on / off, the modes.
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

// ------------------------------------------------------- thermostat page --

namespace {
constexpr int kThermoRingMargin = 18;    // the ring's touch area beyond the beads (layout.py THERMO_RING_MARGIN)
constexpr int kThermoBead = 7;           // a bead's radius
constexpr int kThermoKnob = 11;          // the knob's
constexpr int kThermoPitch = 3;          // px between two beads' middles (5 showed the beads' edges)
constexpr int kThermoButton = 24;        // minus and plus: their discs' radius (layout.py THERMO_BUTTON_R)
constexpr int kThermoDisc = 14855;       // ... their colour, the pills' (#3c423c)
constexpr int kThermoTrack = 19017;      // #4a4a4a: the ring where it is not filled
constexpr int kIconFont = 7;             // the icon font at 24 px (ohpanel_hmi.py ICON_FONT)
constexpr int kHeating = 64615, kCooling = 20031;   // #ff8a3d, #4fc3f7
constexpr int64_t kThermoSentMs = 6000;  // a target sent shows this long at most before the item's comes back
constexpr int64_t kThermoDragHoldMs = 1500;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRingFrom = 0.75 * kPi, kRingSweep = 1.5 * kPi;   // from the lower left, clockwise, open below

int rgb(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

/* x (0..1) to the power `curve` -- one of 0.25, 0.5, 0.75, 1, 1.5, 2 -- from square roots (std::pow costs 3 KB). */
double curved(double x, double curve) {
  const double r = std::sqrt(x), rr = std::sqrt(r);
  switch (static_cast<int>(std::lround(curve * 4))) {
    case 1: return rr;
    case 2: return r;
    case 3: return r * rr;
    case 6: return x * r;
    case 8: return x * x;
    default: return x;
  }
}

/* The ring of the page (its touch area, a square around it): middle and radius. */
struct Ring {
  int cx = 0, cy = 0, r = 0, beads = 0;
};
Ring ring_of(int x, int y, int w, int h) {
  Ring g;
  g.cx = x + w / 2;
  g.cy = y + h / 2;
  g.r = w / 2 - kThermoRingMargin;
  g.beads = std::max(2, static_cast<int>(std::lround(kRingSweep * g.r / kThermoPitch)) + 1);
  return g;
}
std::pair<int, int> bead_at(const Ring &g, int i) {
  const double a = kRingFrom + kRingSweep * i / (g.beads - 1);
  return {g.cx + static_cast<int>(std::lround(g.r * std::cos(a))), g.cy + static_cast<int>(std::lround(g.r * std::sin(a)))};
}
}  // namespace

/* The target's ends and step: the page's, else the item's in openHAB (its stateDescription), else 5 .. 30 by 0.5. */
Engine::ThermoRange Engine::thermo_range(const cJSON *page) const {
  ThermoRange r;
  appoh::Limits l;
  const bool known = cb_.limits && cb_.limits(str(page, "item"), l);
  r.min = num_of(page, "min", known && l.has_min ? l.min : 5);
  r.max = num_of(page, "max", known && l.has_max ? l.max : 30);
  r.step = num_of(page, "step", known && l.has_step ? l.step : 0.5);
  if (r.step <= 0)
    r.step = 0.5;
  if (r.max <= r.min)
    r.max = r.min + r.step;
  return r;
}

/* The modes to choose from: the page's ("modes": label, value), else the mode item's options in openHAB; 4 at most. */
std::vector<std::pair<std::string, std::string>> Engine::thermo_modes(const cJSON *page) const {
  std::vector<std::pair<std::string, std::string>> out;
  const cJSON *mode = member(page, "mode");
  if (str(mode, "item").empty())
    return out;
  if (const cJSON *list = member(page, "modes"); cJSON_IsArray(list)) {
    for (const cJSON *e = list->child; e && out.size() < 4; e = e->next)
      out.emplace_back(str(e, "label", str(e, "value")), str(e, "value"));
    return out;
  }
  std::vector<appoh::Option> options;
  if (cb_.options && cb_.options(str(mode, "item"), options, false))
    for (const auto &o : options) {
      if (out.size() >= 4)
        break;
      out.emplace_back(o.label.empty() ? o.value : o.label, o.value);
    }
  return out;
}

double Engine::thermo_target(const cJSON *page) const {
  if (thermo_drag_.live(now_ms_))
    return thermo_drag_.value;
  if (thermo_sent_.live(now_ms_))
    return thermo_sent_.value;
  const appoh::ItemState s = state_of(page);
  return has_state(page) && s.has_numeric && !s.undefined() ? s.numeric : NAN;
}

/* "21,5 °C": as many decimals as the step has, the item's unit (else °). */
std::string Engine::thermo_text(const cJSON *page, double v) const {
  const ThermoRange r = thermo_range(page);
  const int decimals = std::fabs(r.step - std::round(r.step)) < 1e-6 ? 0 : std::fabs(r.step * 10 - std::round(r.step * 10)) < 1e-6 ? 1 : 2;
  const std::string unit = unit_of(state_of(page));
  return format_number(v, decimals, false, settings_.decimal) + (unit.empty() ? "°" : " " + unit);
}

std::string Engine::thermo_key(const cJSON *page) const {
  const ThermoRange r = thermo_range(page);
  char buf[64];   // (std::to_string of a double would bring the C++ library's to_chars tables: 100 KB)
  std::snprintf(buf, sizeof(buf), "%g|%g|%g", r.min, r.max, r.step);
  std::string key = buf;
  for (const auto &[label, value] : thermo_modes(page))
    key += "|" + label + "=" + value;
  return key;
}

/* The mode's rules ("mode": {"value_style": [{"equals": "schedule", "locked": true}]}): its target not to be set now
   -- no minus and plus, the ring not to be dragged. */
bool Engine::thermo_locked(const cJSON *page) const {
  const cJSON *mode = member(page, "mode");
  return thermo_off(page) || (member(mode, "value_style") && has_state(mode) &&
                              rule_style(member(mode, "value_style"), state_of(mode), now_).locked == 1);
}

/* Its "switch" item is off (not while that is not known) and "locked": the target not set, the ring grey, no modes
   lit. Without "locked" the switch is only its pill. */
bool Engine::thermo_off(const cJSON *page) const {
  const cJSON *sw = member(page, "switch");
  return flag(sw, "locked") && !str(sw, "item").empty() && has_state(sw) && !state_of(sw).undefined() && !on(sw);
}

void Engine::draw_thermostat(const cJSON *page) {
  thermo_drawn_key_ = thermo_key(page);
  txt("title", str(page, "title"));
  thermo_beads_.clear();
  thermo_knob_ = -1;
  thermo_drag_.clear();
  thermo_touching_ = false;
  thermo_swiped_ = false;
  // minus and plus: the icon on its disc (drawn by thermo_refresh, after the ring: what is drawn right as the page
  // loads, the display paints its background over)
  for (const char *n : {"minus", "plus"}) {   // shown by thermo_refresh unless locked
    txt(n, icon_glyph(n));   // the design's fields wait for their icon
    num(n, "bco", kThermoDisc);
  }
  for (const char *part : {"now", "target", "ring_hs"})
    vis(part, true);
  vis("power_hs", !str(member(page, "switch"), "item").empty());   // on / off: its pill (thermo_refresh)
  // the modes' touch areas: the set for as many as there are (the pills: thermo_refresh)
  const size_t n = thermo_modes(page).size();
  for (size_t k = 1; k <= n; k++)
    vis("m" + std::to_string(n) + std::to_string(k) + "_hs", true);
  bind(page, [this, page] { thermo_refresh(page); });
  for (const char *ref : {"current", "heating", "cooling", "mode", "switch"})
    if (const std::string item = str(member(page, ref), "item"); !item.empty())
      bindings_.push_back({item, [this, page] { thermo_refresh(page); }});
}

/* What the items say now: the temperature, the target (the finger's while it sets one), heating or cooling, the
   ring, the mode. */
void Engine::thermo_refresh(const cJSON *page) {
  const Texts &t = settings_.texts;
  const ThermoRange r = thermo_range(page);
  // the temperature now: its number large, with a degree sign
  const cJSON *current = member(page, "current");
  const appoh::ItemState now = state_of(current);
  if (!str(current, "item").empty())
    // a space in front: the number centred as if the degree sign were not there (in font 6 a space is 15 px, the
    // degree sign 22)
    txt("now", has_state(current) && now.has_numeric && !now.undefined()
                   ? " " + format_number(now.numeric, 1, false, settings_.decimal) + "°" : t.undefined);
  else
    txt("now", "");
  // off: "off" for the target, the ring grey (as without a target)
  const bool off = thermo_off(page);
  const double target = thermo_target(page);
  txt("target", off ? t.off : t.thermostat_target + " " + (std::isnan(target) ? t.undefined : thermo_text(page, target)));
  thermo_ring(page, off || std::isnan(target) ? -1 : std::clamp((target - r.min) / (r.max - r.min), 0.0, 1.0));
  const auto pg = boxes_.find("thermostat");
  // heating, cooling: either, its icon and its word in its colour, centred together -- drawn (a field's box would
  // reach into the ring), the background back first
  const bool heating = !off && !str(member(page, "heating"), "item").empty() && on(member(page, "heating"));
  const bool cooling = !off && !heating && !str(member(page, "cooling"), "item").empty() && on(member(page, "cooling"));
  if (pg != boxes_.end() && pg->second.count("state_hs")) {
    const Box &b = pg->second.at("state_hs");
    const std::string word = heating ? t.heating : cooling ? t.cooling : "";
    const std::string key = std::to_string(heating) + std::to_string(cooling) + word;
    if (drawn_["state"] != key) {
      drawn_["state"] = key;
      restore(b.x, b.y, b.w, b.h);
      if (!word.empty()) {
        const int colour = heating ? kHeating : kCooling, icon_w = 24, gap = 4;
        const int tw = std::min(text_width(1, word), b.w - icon_w - gap);
        const int x = b.x + (b.w - (icon_w + gap + tw)) / 2;
        send("xstr " + std::to_string(x) + "," + std::to_string(b.y) + "," + std::to_string(icon_w) + "," +
             std::to_string(b.h) + "," + std::to_string(kIconFont) + "," + std::to_string(colour) + ",0,1,1,3,\"" +
             nextion::quote(icon_glyph(heating ? "fire" : "snowflake")) + "\"");
        // the word on the icon's bottom: the icon font's ink ends 22 px down its 24, font 1's baseline is 15 down its 18
        send("xstr " + std::to_string(x + icon_w + gap) + "," + std::to_string(b.y + 22 - 15) + "," +
             std::to_string(tw + 2) + ",18,1," + std::to_string(colour) + ",0,0,0,3,\"" + nextion::quote(word) + "\"");
      }
    }
  }
  // minus and plus: a disc each under its icon -- none while the mode locks the target (the background back)
  const bool locked = thermo_locked(page);
  for (const char *n : {"minus", "plus"}) {
    const std::string hs = std::string(n) + "_hs";
    if (pg != boxes_.end() && pg->second.count(hs) && drawn_[n] != (locked ? "none" : "disc")) {
      const Box &b = pg->second.at(hs);
      if (locked) {
        vis(n, false);
        if (drawn_[n] == "disc")
          restore(b.x, b.y, b.w, b.h);
      } else {
        disc(b.x + b.w / 2, b.y + b.h / 2, kThermoButton, kThermoDisc);
        vis(n, true);
        send("ref " + std::string(n));   // the icon over it again
      }
      drawn_[n] = locked ? "none" : "disc";
    }
    vis(hs, !locked);
    num(n, "pco", kWhite);   // also after a touch coloured it (the display's own)
  }
  // the switch: a pill, its icon ("power") and label (on / off), in the accent while its item is on
  if (const cJSON *sw = member(page, "switch"); !str(sw, "item").empty() && pg != boxes_.end() &&
                                                 pg->second.count("power_hs")) {
    const bool lit = !has_state(sw) || state_of(sw).undefined() || on(sw);   // not known yet: as on
    const int colour = lit ? theme_.accent : kDarkGrey, ink = lit ? kWhite : kLightGrey;
    const std::string label = str(sw, "label", lit ? t.on : t.off), icon = str(sw, "icon", "power");
    const std::string key = std::to_string(colour) + "|" + label + "|" + icon;
    if (drawn_["power"] != key) {
      drawn_["power"] = key;
      pill(pg->second.at("power_hs"), colour, ink, label, icon_glyph(icon.c_str()));
    }
  }
  // the modes: a pill each, its label on it; the one it is in (or just tapped) in the accent -- none while off
  const auto modes = thermo_modes(page);
  const std::string mode = has_state(member(page, "mode")) ? state_of(member(page, "mode")).state : "";
  for (size_t k = 0; pg != boxes_.end() && k < modes.size(); k++) {
    const std::string n = "m" + std::to_string(modes.size()) + std::to_string(k + 1);
    if (!pg->second.count(n + "_hs"))
      continue;
    const bool at = (!off && mode == modes[k].second && !mode_tapped_.set) ||
                    (mode_tapped_.live(now_ms_) && mode_tapped_.value == static_cast<int>(k));
    const int colour = at ? theme_.accent : kDarkGrey, ink = at ? kWhite : kLightGrey;
    const Box &bx = pg->second.at(n + "_hs");
    const std::string label = shorten(2, modes[k].first, bx.w - 12, false);
    const std::string key = std::to_string(colour) + "|" + label;
    if (drawn_[n] == key)
      continue;
    drawn_[n] = key;
    pill(bx, colour, ink, label);
  }
}

/* The ring: beads along three quarters of a circle, in the accent (or blue to red: "style": "gradient") as far as
   the target, the track's grey beyond; a white knob where it ends. Only the beads that change are drawn again;
   where the knob was, the background comes back first (picq) and the beads under it are drawn again. */
void Engine::thermo_ring(const cJSON *page, double share) {
  const auto pg = boxes_.find("thermostat");
  if (pg == boxes_.end() || !pg->second.count("ring_hs"))
    return;
  const Box &rb = pg->second.at("ring_hs");
  const Ring g = ring_of(rb.x, rb.y, rb.w, rb.h);
  if (static_cast<int>(thermo_beads_.size()) != g.beads)
    thermo_beads_.assign(g.beads, -1);
  const bool gradient = str(page, "style") == "gradient";
  const int knob = share < 0 ? -1 : static_cast<int>(std::lround(share * (g.beads - 1)));
  const auto colour = [&](int i) {
    if (knob < 0 || i > knob)
      return kThermoTrack;
    if (!gradient)
      return theme_.accent;
    // #00a0ff to #ff2828 along the ring, by its "curve" (the share's power: below 1 red the sooner; default 0.5)
    const double f = curved(static_cast<double>(i) / (g.beads - 1), num_of(page, "curve", 0.5));
    return rgb(static_cast<int>(255 * f), static_cast<int>(160 - 120 * f), static_cast<int>(255 - 215 * f));
  };
  bool redraw_knob = knob != thermo_knob_;
  if (thermo_knob_ >= 0 && knob != thermo_knob_) {   // the old knob away: the background, the beads under it again
    const auto [kx, ky] = bead_at(g, thermo_knob_);
    restore(kx - kThermoKnob - 1, ky - kThermoKnob - 1, 2 * kThermoKnob + 3, 2 * kThermoKnob + 3);
    for (int i = 0; i < g.beads; i++) {
      const auto [bx, by] = bead_at(g, i);
      if (std::abs(bx - kx) <= kThermoKnob + kThermoBead + 1 && std::abs(by - ky) <= kThermoKnob + kThermoBead + 1)
        thermo_beads_[i] = -1;
    }
  }
  for (int i = 0; i < g.beads; i++) {
    const int c = colour(i);
    if (thermo_beads_[i] == c)
      continue;
    thermo_beads_[i] = c;
    const auto [bx, by] = bead_at(g, i);
    disc(bx, by, kThermoBead, c);
    if (knob >= 0 && std::abs(i - knob) <= 2 * (kThermoKnob + kThermoBead) / kThermoPitch)
      redraw_knob = true;   // drawn over it: the knob again, on top
  }
  if (knob >= 0 && redraw_knob) {
    const auto [kx, ky] = bead_at(g, knob);
    disc(kx, ky, kThermoKnob, kWhite);
  }
  thermo_knob_ = knob;
}

/* A target to the item: within the range, on its step, with the item's unit; shown at once. */
void Engine::thermo_send(const cJSON *page, double v) {
  const ThermoRange r = thermo_range(page);
  v = std::clamp(r.min + std::round((v - r.min) / r.step) * r.step, r.min, r.max);
  thermo_sent_.hold(v, now_ms_, kThermoSentMs);
  if (cb_.command && !str(page, "item").empty()) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string number = buf;
    number.erase(number.find_last_not_of('0') + 1);
    if (!number.empty() && number.back() == '.')
      number.pop_back();
    const std::string unit = state_of(page).unit;
    cb_.command(str(page, "item"), unit.empty() ? number : number + " " + unit);
  }
  thermo_refresh(page);
}

void Engine::thermo_event(const cJSON *page, const std::string &component) {
  sent_.erase("minus.pco");   // a touch coloured the icon on the display itself
  sent_.erase("plus.pco");
  if ((component == "minus" || component == "plus") && thermo_locked(page)) {
    thermo_refresh(page);
    return;
  }
  if (component == "minus" || component == "plus") {
    const ThermoRange r = thermo_range(page);
    const double base = thermo_target(page);
    if (!std::isnan(base))
      thermo_send(page, base + (component == "plus" ? r.step : -r.step));
    else
      thermo_refresh(page);
    return;
  }
  if (component == "power") {   // on / off: the other one to its item
    const cJSON *sw = member(page, "switch");
    if (cb_.command && !str(sw, "item").empty())
      cb_.command(str(sw, "item"), toggle_command(sw));
    thermo_refresh(page);
    return;
  }
  if (component.rfind("mode", 0) == 0 && component.size() == 5) {
    const int k = component[4] - '1';
    const auto modes = thermo_modes(page);
    if (k >= 0 && k < static_cast<int>(modes.size()) && cb_.command) {
      cb_.command(str(member(page, "mode"), "item"), modes[k].second);
      mode_tapped_.hold(k, now_ms_, 3000);   // lit at once, until the item says so (thermo_tick) or a while has passed
    }
  }
  thermo_refresh(page);
}

/* The finger on the ring ("tdrw", while it moves too) or let go ("tdru"): x * 1000 + y, read as the angle round
   the ring -- in its opening, the nearer end. A touch has to begin on the ring, not inside it. Shown at once; sent
   when let go. One that goes on sideways off the ring is a swipe (the page in the swipe row next to it): the
   target as it was, nothing sent -- along the ring's top a finger moves sideways too, but stays on it. */
void Engine::thermo_value(const std::string &name, int value) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  const auto pg = boxes_.find("thermostat");
  if (!page || pg == boxes_.end() || !pg->second.count("ring_hs") || (name != "tdrw" && name != "tdru"))
    return;
  if (thermo_locked(page))
    return;   // the mode keeps the target
  const Box &rb = pg->second.at("ring_hs");
  const Ring g = ring_of(rb.x, rb.y, rb.w, rb.h);
  const double dx = value / 1000 - g.cx, dy = value % 1000 - g.cy;
  if (!thermo_touching_) {
    if (std::hypot(dx, dy) < g.r - 2.5 * kThermoBead)
      return;   // inside the ring: not for it
    thermo_touching_ = true;
    thermo_from_ = value;
    thermo_swiped_ = false;
  }
  if (thermo_swiped_) {
    if (name == "tdru")
      thermo_touching_ = false;
    return;
  }
  const int mx = std::abs(value / 1000 - thermo_from_ / 1000), my = std::abs(value % 1000 - thermo_from_ % 1000);
  if (mx >= 40 && mx > 2 * my && std::fabs(std::hypot(dx, dy) - g.r) > 2.5 * kThermoBead) {
    thermo_swiped_ = true;
    thermo_touching_ = name != "tdru";
    thermo_drag_.clear();
    thermo_refresh(page);
    return;
  }
  double a = std::atan2(dy, dx);
  while (a < kRingFrom)
    a += 2 * kPi;
  double share = (a - kRingFrom) / kRingSweep;
  if (share > 1)
    share = share < 1 + (2 * kPi - kRingSweep) / kRingSweep / 2 ? 1 : 0;   // in the opening: the nearer end
  const ThermoRange r = thermo_range(page);
  thermo_drag_.hold(std::clamp(r.min + std::round(share * (r.max - r.min) / r.step) * r.step, r.min, r.max), now_ms_,
                    kThermoDragHoldMs);
  if (name == "tdru") {
    thermo_touching_ = false;
    const double v = thermo_drag_.value;
    thermo_drag_.clear();
    thermo_send(page, v);
    return;
  }
  thermo_refresh(page);
}

/* A target sent, a mode tapped: once the item says so (or a while has passed), what it says. */
void Engine::thermo_tick() {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || shown_ != "thermostat")
    return;
  bool again = false;
  if (thermo_sent_.set) {
    const appoh::ItemState s = state_of(page);
    if (thermo_sent_.due(now_ms_) || (s.has_numeric && std::fabs(s.numeric - thermo_sent_.value) < 1e-3)) {
      thermo_sent_.clear();
      again = true;
    }
  }
  if (mode_tapped_.set) {
    const auto modes = thermo_modes(page);
    const std::string mode = state_of(member(page, "mode")).state;
    if (mode_tapped_.due(now_ms_) ||
        (mode_tapped_.value < static_cast<int>(modes.size()) && mode == modes[mode_tapped_.value].second)) {
      mode_tapped_.clear();
      again = true;
    }
  }
  if (thermo_drag_.due(now_ms_)) {   // a drag that never ended
    thermo_drag_.clear();
    thermo_touching_ = false;
    again = true;
  }
  if (again)
    thermo_refresh(page);
}



}  // namespace appdisplay
