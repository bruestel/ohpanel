/*
   The cover page and the cover group page (Engine::draw_cover, draw_covers).
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

// ------------------------------------------------------------ cover page --
//
// boards/nspanel/nextion/ohpanel_hmi.py cover_page / covers_page, layout.py COVER_*, COVERS_*: a cover's position
// large, the cover drawn in its style (a window and its shutter, a screen, an awning seen from above, a curtain,
// a garage door, a sliding gate) -- the firmware draws it, dragging on it sets the position --, up / stop / down,
// presets; a group's members as rows with a small drawing each.

namespace {
constexpr int c565(int r, int g, int b) { return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); }
constexpr int kCoverFrame = c565(0x4a, 0x44, 0x50), kSkyTop = c565(0x8f, 0xc0, 0xea), kSkyLow = c565(0xc3, 0xde, 0xf4);
constexpr int kSlat = c565(0xcf, 0xc8, 0xd6), kSlatGap = c565(0x9d, 0x95, 0xa8), kCoverBar = c565(0x6d, 0x65, 0x77);
constexpr int kFabric = c565(0xa3, 0x98, 0x86), kFabricLine = c565(0x8a, 0x7e, 0x6e), kFabricBar = c565(0x5a, 0x50, 0x48);
constexpr int kWall = c565(0x5a, 0x52, 0x62), kDeck = c565(0x3a, 0x34, 0x40), kDeckLine = c565(0x43, 0x3c, 0x4a);
constexpr int kCloth = c565(0xef, 0xe7, 0xf5);
constexpr int kCurtain = c565(0xb9, 0xa3, 0xc9), kCurtainFold = c565(0x9e, 0x86, 0xb0);
constexpr int kDoor = c565(0xa5, 0x9b, 0xb2), kDoorSeam = c565(0x7d, 0x73, 0x8a), kGarageDark = c565(0x1d, 0x1a, 0x22);
constexpr int kGate = c565(0xb8, 0xae, 0xc6), kGround = c565(0x3c, 0x36, 0x44);
constexpr int kCoverButton = c565(0x3a, 0x3a, 0x40);   // the group page's small buttons (ohpanel_hmi.py COVER_BUTTON)
constexpr int64_t kCoverMovingMs = 3000;   // seen moving (or commanded): shown so this long after the last change
constexpr int64_t kCoverDragHoldMs = 2000;   // after a drag the item's echoes do not move the drawing back
const char *const kCoverStyles[] = {"shutter", "screen", "awning", "curtain", "garage", "gate"};
// the tiles' icons per style: open, stop, close
/* A tile's icon (0 open, 1 stop, 2 close) for a style: a gate from the left opens to the left. */
const char *cover_icon(int style, int k, bool from_left);
const char *const kCoverIcons[][3] = {{"arrow-up", "stop", "arrow-down"},
                                      {"arrow-up", "stop", "arrow-down"},
                                      {"arrow-up", "stop", "arrow-down"},
                                      {"arrow-expand-horizontal", "stop", "arrow-collapse-horizontal"},
                                      {"arrow-up", "stop", "arrow-down"},
                                      {"arrow-right", "stop", "arrow-left"}};

const char *cover_icon(int style, int k, bool from_left) {
  if (style == 5 && from_left && k != 1)
    return k == 0 ? "arrow-left" : "arrow-right";
  return kCoverIcons[style][k];
}

/* "Sun=70, Air=85" (metadata "presets"): label, position; three at most. */
std::vector<std::pair<std::string, int>> presets_of(const std::string &text) {
  std::vector<std::pair<std::string, int>> out;
  size_t at = 0;
  while (at < text.size() && out.size() < 3) {
    const size_t end = std::min(text.find(',', at), text.size());
    const std::string part = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = part.rfind('=');
    if (eq == std::string::npos)
      continue;
    const auto trim = [](std::string v) {
      v.erase(0, v.find_first_not_of(' '));
      v.erase(v.find_last_not_of(' ') + 1);
      return v;
    };
    const std::string label = trim(part.substr(0, eq)), number = trim(part.substr(eq + 1));
    char *e = nullptr;
    const long v = std::strtol(number.c_str(), &e, 10);
    if (!label.empty() && e && *e == '\0' && !number.empty() && v >= 0 && v <= 100)
      out.emplace_back(label, static_cast<int>(v));
  }
  return out;
}
}  // namespace

std::string Engine::cover_look_key(const CoverLook &l) {
  std::string key = l.style + (l.reverse ? "|r" : "|") + (l.from_left ? "l|" : "|") + l.moving + "|" + l.label;
  for (const auto &[label, pos] : l.presets)
    key += "|" + label + "=" + std::to_string(pos);
  return key;
}

int Engine::cover_style_index(const std::string &style) {
  for (int k = 0; k < 6; k++)
    if (style == kCoverStyles[k])
      return k;
  return 0;
}

/* The page's look: its own "style", "reverse", "presets" -- else the item's metadata in the page's namespace
   ("metadata", default "cover": style, reverse, presets "Sun=70, Air=85") -- else a shutter, no presets. */
Engine::CoverLook Engine::cover_look(const cJSON *page) const {
  CoverLook l;
  std::map<std::string, appoh::Metadata> md;
  const std::string ns = str(page, "metadata", "cover");
  const appoh::Metadata *m = nullptr;
  // a group page: the group's own metadata (its presets, for the whole group)
  if (cb_.metadata && cb_.metadata(str(page, "item", str(page, "group")), md, false) && md.count(ns))
    m = &md.at(ns);
  const auto conf = [&](const char *key) -> std::string {
    if (!m)
      return "";
    const auto it = m->config.find(key);
    return it == m->config.end() ? "" : it->second;
  };
  l.style = kCoverStyles[cover_style_index(str(page, "style", conf("style")))];
  l.reverse = cJSON_GetObjectItem(page, "reverse") ? flag(page, "reverse") : conf("reverse") == "true";
  l.from_left = str(page, "from", conf("from")) == "left";
  l.moving = str(member(page, "moving"), "item", conf("moving"));
  l.command_item = str(member(page, "commands"), "item", conf("command_item"));
  l.label = conf("label");
  if (const cJSON *p = member(page, "presets"); cJSON_IsArray(p)) {
    for (const cJSON *e = p->child; e && l.presets.size() < 3; e = e->next)
      l.presets.emplace_back(str(e, "label"), std::clamp(static_cast<int>(num_of(e, "position", 0)), 0, 100));
  } else {
    l.presets = presets_of(conf("presets"));
  }
  return l;
}

/* A member's look on a group page: its metadata in the page's namespace, else the page's style. */
Engine::CoverLook Engine::member_look(const cJSON *group_page, const appoh::Member &mb) const {
  CoverLook l;
  const auto it = mb.metadata.find(str(group_page, "metadata", "cover"));
  const auto conf = [&](const char *key) -> std::string {
    if (it == mb.metadata.end())
      return "";
    const auto c = it->second.config.find(key);
    return c == it->second.config.end() ? "" : c->second;
  };
  const std::string style = conf("style");
  l.style = kCoverStyles[cover_style_index(style.empty() ? str(group_page, "style") : style)];
  l.reverse = conf("reverse") == "true";
  l.from_left = (conf("from").empty() ? str(group_page, "from") : conf("from")) == "left";
  l.presets = presets_of(conf("presets"));
  l.label = conf("label").empty() ? mb.label : conf("label");
  l.moving = conf("moving");
  l.command_item = conf("command_item");
  return l;
}

int Engine::cover_closed(const std::string &item, bool reverse) const {
  appoh::ItemState s;
  if (item.empty() || !cb_.state || !cb_.state(item, s) || !s.has_numeric)
    return -1;
  int p = std::clamp(static_cast<int>(std::lround(s.numeric)), 0, 100);
  if (p <= 1) p = 0;   // a sensor's 1 % at the end: there
  if (p >= 99) p = 100;
  return reverse ? 100 - p : p;
}

/* The cover in its style in the box, closed this far (0..100; -1: not known, drawn open). `bg`: the colour
   around it (a row's card), -1: the page's background (bg_pic, cropped back where nothing is drawn). Drawn
   whole each time, each part once -- no flicker. */
constexpr int64_t kCoverSettleMs = 2500;

int Engine::cover_settled(const std::string &item, bool reverse) {
  const int raw = cover_closed(item, false);   // the item's terms
  if (raw < 0)
    return -1;
  Settle &s = settle_[item];
  const bool jump = s.shown >= 0 && std::abs(raw - s.shown) > 10;   // a cover moves a few % a second
  if (!jump) {
    s.shown = raw;
    s.pending = -1;
  } else if (s.pending != raw) {
    s.pending = raw;
    s.since_ms = now_ms_;
  } else if (now_ms_ - s.since_ms >= kCoverSettleMs) {
    s.shown = raw;
    s.pending = -1;
  }
  return reverse ? 100 - s.shown : s.shown;
}

void Engine::draw_cover_art(int x, int y, int w, int h, const std::string &style, int closed, int bg,
                            bool from_left) {
  const bool mini = w < 60;
  closed = std::clamp(closed, 0, 100);
  const auto clear = [&](int fx, int fy, int fw, int fh) {   // what is around the drawing: background
    if (bg >= 0)
      fill(fx, fy, fw, fh, bg);
    else
      restore(fx, fy, fw, fh);
  };
  // stripes across (horizontal: rows of `pitch`, `line` of them in `b`) or along
  const auto rows = [&](int fx, int fy, int fw, int fh, int pitch, int line, int a, int b) {
    for (int yy = 0; yy < fh; yy += pitch) {
      const int body = std::min(pitch - line, fh - yy);
      fill(fx, fy + yy, fw, body, a);
      fill(fx, fy + yy + body, fw, std::min(line, fh - yy - body), b);
    }
  };
  const auto cols = [&](int fx, int fy, int fw, int fh, int pitch, int line, int a, int b) {
    for (int xx = 0; xx < fw; xx += pitch) {
      const int body = std::min(pitch - line, fw - xx);
      fill(fx + xx, fy, body, fh, a);
      fill(fx + xx + body, fy, std::min(line, fw - xx - body), fh, b);
    }
  };
  const auto frame = [&](int b, int c) {   // a window's frame, its inside returned
    fill(x, y, w, b, c);
    fill(x, y + h - b, w, b, c);
    fill(x, y + b, b, h - 2 * b, c);
    fill(x + w - b, y + b, b, h - 2 * b, c);
  };
  const auto sky = [&](int fx, int fy, int fw, int fh, int top, int height) {   // light sky, lighter low down
    const int mid = top + height / 2;
    const int a = std::clamp(mid - fy, 0, fh);
    fill(fx, fy, fw, a, kSkyTop);
    fill(fx, fy + a, fw, fh - a, kSkyLow);
  };
  const int b = mini ? 2 : 4;
  const int ix = x + b, iy = y + b, iw = w - 2 * b, ih = h - 2 * b;
  /* The grip where the finger takes it: a pill in the accent across the moving edge (along it for a curtain and
     a gate), two light lines on it -- on the large drawing only. */
  const auto grip = [&](int cx, int cy, bool across) {
    if (mini)
      return;
    const int len = 44, thick = 14, r = thick / 2;
    const int gw = across ? len : thick, gh = across ? thick : len, gx = cx - gw / 2, gy = cy - gh / 2;
    fill(across ? gx + r : gx, across ? gy : gy + r, across ? gw - 2 * r : gw, across ? gh : gh - 2 * r, theme_.accent);
    for (const int e : {0, 1}) {
      const int ex = across ? (e ? gx + gw - 1 - r : gx + r) : cx, ey = across ? cy : (e ? gy + gh - 1 - r : gy + r);
      disc(ex, ey, r, theme_.accent);
    }
    for (const int d : {-3, 2})   // the two lines
      fill(across ? cx - 9 : cx + d, across ? cy + d : cy - 9, across ? 18 : 1, across ? 1 : 18, kWhite);
  };
  if (style == "awning") {   // seen from above: the wall at the top, the cloth out over the terrace's boards
    const int wall = mini ? 4 : 12, side = mini ? 2 : 8, bar = mini ? 2 : 6;
    fill(x, y, w, wall, kWall);
    const int room = h - wall, out = room * closed / 100;
    if (out > 0) {
      fill(x, y + wall, side, out, kDeck);
      fill(x + w - side, y + wall, side, out, kDeck);
      cols(x + side, y + wall, w - 2 * side, std::max(0, out - bar), mini ? 6 : 26, mini ? 3 : 13, theme_.accent,
           kCloth);
      fill(x + side / 2, y + wall + out - bar, w - side, bar, kCoverBar);
    }
    cols(x, y + wall + out, w, room - out, mini ? 8 : 24, mini ? 1 : 2, kDeck, kDeckLine);
    grip(x + w / 2, std::clamp(y + wall + out - bar / 2, y + wall + 8, y + h - 8), true);
  } else if (style == "curtain") {   // from both sides
    frame(b, kCoverFrame);
    const int each = iw * (8 + 42 * closed / 100) / 100;
    const int pitch = mini ? 3 : 9, line = mini ? 1 : 4;
    cols(ix, iy, each, ih, pitch, line, kCurtain, kCurtainFold);
    sky(ix + each, iy, iw - 2 * each, ih, iy, ih);
    cols(ix + iw - each, iy, each, ih, pitch, line, kCurtain, kCurtainFold);
    grip(ix + each, iy + ih / 2, false);
    grip(ix + iw - each, iy + ih / 2, false);
  } else if (style == "garage") {
    frame(b, kWall);
    const int down = ih * closed / 100;
    rows(ix, iy, iw, down, mini ? 6 : 21, mini ? 1 : 3, kDoor, kDoorSeam);
    fill(ix, iy + down, iw, ih - down, kGarageDark);
    grip(ix + iw / 2, std::clamp(iy + down - 3, iy + 8, iy + ih - 8), true);
  } else if (style == "gate") {   // a sliding gate between two posts, coming in from the right
    const int post = mini ? 3 : 12, ground = mini ? 2 : 6, top = y + h / 4;
    fill(x, y, post, h, kWall);
    fill(x + w - post, y, post, h, kWall);
    const int gx = x + post, gw = w - 2 * post, gh = y + h - ground - top;
    fill(gx, y + h - ground, gw, ground, kGround);
    clear(gx, y, gw, top - y);
    const int in = gw * closed / 100;
    clear(from_left ? gx + in : gx, top, gw - in, gh);
    if (in > 0) {   // the gate: rails top and bottom, bars between them
      const int rail = mini ? 2 : 6, pitch = mini ? 5 : 22, bar = mini ? 1 : 6;
      const int x0 = from_left ? gx : gx + gw - in;
      fill(x0, top, in, rail, kGate);
      fill(x0, top + gh - rail, in, rail, kGate);
      for (int xx = 0; xx < in; xx += pitch) {
        fill(x0 + xx, top + rail, std::min(bar, in - xx), gh - 2 * rail, kGate);
        clear(x0 + xx + bar, top + rail, std::min(pitch - bar, in - xx - bar), gh - 2 * rail);
      }
    }
    grip(std::clamp(from_left ? gx + in : gx + gw - in, gx + 8, gx + gw - 8), top + gh / 2, false);
  } else {   // a shutter (slats) or a screen (cloth) coming down a window
    frame(b, kCoverFrame);
    const bool screen = style == "screen";
    const int down = ih * closed / 100, bar = mini ? 2 : 6;
    if (screen)
      cols(ix, iy, iw, std::max(0, down - bar), mini ? 3 : 5, mini ? 1 : 2, kFabric, kFabricLine);
    else
      rows(ix, iy, iw, std::max(0, down - bar), mini ? 4 : 11, mini ? 1 : 2, kSlat, kSlatGap);
    if (down > 0)
      fill(ix, iy + std::max(0, down - bar), iw, std::min(bar, down), screen ? kFabricBar : kCoverBar);
    sky(ix, iy + down, iw, ih - down, iy, ih);
    grip(ix + iw / 2, std::clamp(iy + down - bar / 2, iy + 8, iy + ih - 8), true);   // on the bar, in the window
  }
}

void Engine::draw_cover(const cJSON *page) {
  if (member(page, "group")) {
    draw_covers(page);
    return;
  }
  if (!group_asked_ && cb_.metadata && !str(page, "item").empty()) {   // the metadata asked afresh as the page opens
    std::map<std::string, appoh::Metadata> md;                         // (openHAB tells nobody when it changes)
    cb_.metadata(str(page, "item"), md, true);
    group_asked_ = true;
  }
  const CoverLook look = cover_look(page);
  cover_drawn_key_ = cover_look_key(look);
  txt("title", str(page, "title", look.label));   // the page's own title, else its item's metadata's label
  const int si = cover_style_index(look.style);
  for (int k = 0; k < 3; k++) {   // their rounded squares: cover_refresh (lit while it moves that way)
    const std::string n = "btn" + std::to_string(k + 1);
    txt(n + "_icon", icon_glyph(cover_icon(si, k, look.from_left)));
    vis(n + "_icon", true);
    vis(n + "_hs", true);
  }
  for (const char *part : {"value", "state", "draw_hs"})
    vis(part, true);
  // the presets: a pill each, drawn under its label (cover_refresh colours the one the cover is at)
  for (int k = 0; k < 3; k++) {   // the label is drawn on the pill (a field's corners would stand out of it)
    const std::string n = "preset" + std::to_string(k + 1);
    vis(n, false);
    vis(n + "_hs", k < static_cast<int>(look.presets.size()));
  }
  bind(page, [this, page] { cover_refresh(page); });
  if (!look.moving.empty())   // the page's "moving", else its item's metadata's
    bindings_.push_back({look.moving, [this, page] { cover_refresh(page); }});
}

/* What the cover's item (and its "moving" item) says now: the position, the state in words, which way it
   moves (the tile of that way lit), the drawing, the preset it is at -- not while the finger sets it. */
void Engine::cover_refresh(const cJSON *page) {
  const CoverLook look = cover_look(page);
  const int si = cover_style_index(look.style);
  const std::string item = str(page, "item");
  const Texts &t = settings_.texts;
  const bool dragging = cover_drag_.live(now_ms_);
  int closed = cover_settled(item, look.reverse);
  // which way it moves: the "moving" item (UP / DOWN / STOP, the item's terms), else the position changing
  Motion &m = motion_[item];
  if (closed >= 0) {
    const int p = look.reverse ? 100 - closed : closed;
    if (m.last >= 0 && p != m.last) {
      const int seen = p > m.last ? 1 : -1;
      // which way it goes: as seen -- not against a way just sent (a device first tells where it was), nor turned
      // by a step of 1 (a sensor's wobble)
      const bool against = m.dir && seen != m.dir && (now_ms_ < m.commanded_until_ms || std::abs(p - m.last) <= 1);
      if (!against)
        m.dir = seen;
      // arrived at an end: no further; on the way: so for a while after each step
      m.until_ms = now_ms_ + (p == 0 || p == 100 ? 500 : kCoverMovingMs);
    }
    m.last = p;
  }
  int dir = now_ms_ < m.until_ms ? m.dir : 0;
  // the "moving" item: which way, once it says so -- its STOP of before the cover starts (a second or two after the
  // command) does not take back the way just sent
  if (appoh::ItemState mv; !look.moving.empty() && cb_.state && cb_.state(look.moving, mv)) {
    const int said = mv.state == "UP" ? -1 : mv.state == "DOWN" ? 1 : 0;
    if (said || now_ms_ >= m.commanded_until_ms)
      dir = said;
  }
  // at an end nothing moves -- whatever the "moving" item still says: not further that way, nor back (unless it
  // was just sent that way: it starts from there)
  if (closed >= 0) {
    const int p = look.reverse ? 100 - closed : closed;
    if ((p == 0 && dir < 0) || (p == 100 && dir > 0) || ((p == 0 || p == 100) && now_ms_ >= m.commanded_until_ms))
      dir = 0;
  }
  const int shown_dir = look.reverse ? -dir : dir;   // +1: closing as shown
  if (dragging)
    closed = cover_drag_.value;
  txt("value", closed >= 0 ? std::to_string(closed) + " %" : value_of(page));
  txt("state", closed < 0 ? "" : t.cover_words[si][closed == 0 ? 0 : 1]);
  if (shown_dir && !dragging) {
    txt("moving_icon", icon_glyph(cover_icon(si, shown_dir > 0 ? 2 : 0, look.from_left)));
    num("moving_icon", "pco", theme_.accent);
    txt("moving", t.cover_words[si][shown_dir > 0 ? 3 : 2]);
    num("moving", "pco", theme_.accent);
  }
  vis("moving_icon", shown_dir && !dragging);
  vis("moving", shown_dir && !dragging);
  // the buttons, the one of the way it moves lit: a rounded square each (drawn again only when it changes), the
  // icon on it
  const auto pg = boxes_.find("cover");
  for (int k = 0; k < 3 && pg != boxes_.end(); k++) {
    const std::string n = "btn" + std::to_string(k + 1);
    const bool lit = (k == 0 && shown_dir < 0) || (k == 2 && shown_dir > 0);
    const int bg = lit ? kWhite : kDarkGrey;
    if (pg->second.count(n + "_hs") && drawn_[n] != std::to_string(bg)) {
      drawn_[n] = std::to_string(bg);
      const Box &bx = pg->second.at(n + "_hs");
      rounded(bx, 12, bg);
      num(n + "_icon", "bco", bg);
      send("ref " + n + "_icon");
    }
    num(n + "_icon", "pco", lit ? theme_.accent : theme_.dim);   // also after a touch coloured it (the display's own)
  }
  // the drawing, only when it shows something else
  if (pg != boxes_.end() && pg->second.count("draw_hs") && !flag(page, "hide_slider")) {
    const Box &bx = pg->second.at("draw_hs");
    const std::string key = look.style + "|" + std::to_string(closed) + "|" + std::to_string(theme_.accent) +
                            (look.from_left ? "|l" : "");
    if (drawn_["cover_art"] != key) {
      drawn_["cover_art"] = key;
      draw_cover_art(bx.x, bx.y, bx.w, bx.h, look.style, closed < 0 ? 0 : closed, -1, look.from_left);
    }
  }
  // the presets: a pill each, its label on it; the one it is at (or just tapped) in the accent
  for (int k = 0; pg != boxes_.end() && k < static_cast<int>(look.presets.size()); k++) {
    const std::string n = "preset" + std::to_string(k + 1);
    if (!pg->second.count(n + "_hs"))
      continue;
    const bool at = (closed >= 0 && std::abs(closed - look.presets[k].second) <= 1) ||
                    (preset_tapped_.live(now_ms_) && preset_tapped_.value == k);
    const int colour = at ? theme_.accent : kDarkGrey, ink = at ? kWhite : kLightGrey;
    const Box &bx = pg->second.at(n + "_hs");
    const std::string label = shorten(2, look.presets[k].first, bx.w - 12, false);
    const std::string key = std::to_string(colour) + "|" + label;
    if (drawn_[n] == key)
      continue;
    drawn_[n] = key;
    pill(bx, colour, ink, label);
  }
}

/* A command to a cover: open / stop / close (its page's "commands", else UP / STOP / DOWN; a reversed cover's
   open is the item's DOWN), or a position as shown. Open / stop / close go to `command_item` if there is one (a
   String item taking "open", "stop", "close" beside a Dimmer with the position) -- its words as they read, open
   is open whichever way the position runs; the position always goes to `item`. A move shows at once (Motion,
   the item's), until the item tells. */
void Engine::cover_command(const std::string &item, const std::string &which, bool reverse, const cJSON *commands,
                           const std::string &command_item) {
  if (item.empty() || !cb_.command)
    return;
  Motion &m = motion_[item];
  const std::string &to = command_item.empty() ? item : command_item;
  if (which == "stop") {
    cb_.command(to, str(commands, "stop", "STOP"));
    m.until_ms = 0;
    return;
  }
  if (which == "open" || which == "close") {
    const bool up = (which == "open") != reverse;   // the item's UP: towards its 0
    const bool open = command_item.empty() ? up : which == "open";
    cb_.command(to, open ? str(commands, "open", "UP") : str(commands, "close", "DOWN"));
    m.dir = up ? -1 : 1;
    m.until_ms = now_ms_ + kCoverMovingMs;
    m.commanded_until_ms = now_ms_ + 4000;
    return;
  }
  const int shown = std::clamp(std::atoi(which.c_str()), 0, 100);
  const int p = reverse ? 100 - shown : shown;
  cb_.command(item, std::to_string(p));
  if (m.last >= 0 && p != m.last) {
    m.dir = p > m.last ? 1 : -1;
    m.until_ms = now_ms_ + kCoverMovingMs;
    m.commanded_until_ms = now_ms_ + 4000;
  }
}

void Engine::cover_event(const cJSON *page, const std::string &component) {
  // a touch coloured an icon or a label on the display itself: what the firmware sent there is not what shows
  for (const char *n : {"btn1_icon", "btn2_icon", "btn3_icon"})
    sent_.erase(std::string(n) + ".pco");
  const CoverLook look = cover_look(page);
  const std::string item = str(page, "item");
  const cJSON *cmds = member(page, "commands");
  if (component == "open_cover" || component == "stop_cover" || component == "close_cover") {
    cover_command(item, component.substr(0, component.find('_')), look.reverse, cmds, look.command_item);
  } else if (component.rfind("preset", 0) == 0 && component.size() == 7) {
    const int k = component[6] - '1';
    if (k >= 0 && k < static_cast<int>(look.presets.size())) {
      cover_command(item, std::to_string(look.presets[k].second), look.reverse, cmds);
      preset_tapped_.hold(k, now_ms_, 3000);   // lit at once, until the cover is there (cover_refresh) or a while has passed
    }
  }
  cover_refresh(page);
}

/* The finger on the drawing ("cdrw", while it moves too) or let go there ("cdru"): x * 1000 + y, read as the
   position by the style -- how far down the window (from the wall, for the awning), from the sides to the
   middle (curtain), from the right (gate). Shown at once; sent when let go ("live": while it moves too). */
void Engine::cover_value(const std::string &name, int value) {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  const auto pg = boxes_.find("cover");
  if (!page || pg == boxes_.end() || !pg->second.count("draw_hs") || (name != "cdrw" && name != "cdru"))
    return;
  const Box &b = pg->second.at("draw_hs");
  const CoverLook look = cover_look(page);
  const int px = value / 1000, py = value % 1000;
  double share;
  if (look.style == "curtain")
    share = 1 - std::fabs(px - (b.x + b.w / 2.0)) / (b.w / 2.0);
  else if (look.style == "gate")
    share = (look.from_left ? px - b.x : b.x + b.w - px) / static_cast<double>(b.w);
  else if (look.style == "awning")
    share = (py - b.y - 12) / static_cast<double>(b.h - 12);
  else
    share = (py - b.y) / static_cast<double>(b.h);
  int closed = static_cast<int>(std::lround(std::clamp(share, 0.0, 1.0) * 100));
  if (closed <= 3) closed = 0;   // the ends within reach of a finger
  if (closed >= 97) closed = 100;
  cover_drag_.hold(closed, now_ms_, kCoverDragHoldMs);
  cover_refresh(page);
  const bool done = name == "cdru";
  if (!done && (!flag(page, "live", false) || now_ms_ - light_sent_ms_ < kSlideMs))
    return;
  light_sent_ms_ = now_ms_;
  cover_command(str(page, "item"), std::to_string(closed), look.reverse, member(page, "commands"));
}

/* Seen moving a while ago, and no more: the page shows it standing. */
void Engine::cover_tick() {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || (shown_ != "cover" && shown_ != "covers"))
    return;
  for (auto it = button_lit_.begin(); it != button_lit_.end();)   // a group page's button lit a while: back
    if (now_ms_ >= it->second) {
      num(it->first, "pco", kLightGrey);
      it = button_lit_.erase(it);
    } else {
      ++it;
    }
  // a jump to an end that has lasted: shown now
  for (const auto &[item, st] : settle_)
    if (st.pending >= 0 && now_ms_ - st.since_ms >= kCoverSettleMs) {
      for (const auto &b : bindings_)
        if (b.item == item)
          b.draw();
      break;
    }
  if (shown_ == "covers" && preset_tapped_.due(now_ms_)) {   // a tapped preset's light over
    preset_tapped_.clear();
    draw_cover_presets(page);
  }
  if (shown_ != "cover")
    return;
  Motion &m = motion_[str(page, "item")];
  if (m.dir && m.until_ms && now_ms_ >= m.until_ms) {
    m.dir = 0;
    m.until_ms = 0;
    cover_refresh(page);
  }
  if (preset_tapped_.due(now_ms_)) {   // a tapped preset's light over
    preset_tapped_.clear();
    cover_refresh(page);
  }
  if (cover_drag_.due(now_ms_)) {   // the drag over: what the item says
    cover_drag_.clear();
    cover_refresh(page);
  }
}


// ----------------------------------------------------- cover group page --

/* The members of a cover group page, as rows (the whole group has its own, at the top). */
std::vector<std::pair<std::string, appoh::Member>> Engine::cover_rows(const cJSON *page) {
  std::vector<std::pair<std::string, appoh::Member>> out;
  const std::string group = str(page, "group");
  std::vector<appoh::Member> list;
  if (cb_.members && cb_.members(group, list, !group_asked_))
    for (auto &m : list)
      out.push_back({m.name, std::move(m)});
  group_asked_ = true;
  return out;
}

/* The rows a sheet of a cover group page: four, three when the group's presets take the first row's place. */
int Engine::cover_rows_per_sheet(const cJSON *page) const {
  return cover_look(page).presets.empty() ? kCoverRowsPerSheet : kCoverRowsPerSheet - 1;
}

void Engine::draw_covers(const cJSON *page) {
  txt("title", str(page, "title"));
  if (!group_asked_ && cb_.metadata) {   // the group's metadata asked afresh as the page opens (cover_rows: members)
    std::map<std::string, appoh::Metadata> md;
    cb_.metadata(str(page, "group"), md, true);
  }
  const auto rows = cover_rows(page);
  const CoverLook look = cover_look(page);
  covers_drawn_key_ = cover_look_key(look);
  const int first = look.presets.empty() ? 0 : 1;   // the presets in the first row's place
  const int per = kCoverRowsPerSheet - first, sheets = std::max(1, (static_cast<int>(rows.size()) + per - 1) / per);
  sheet_ = std::clamp(sheet_, 0, sheets - 1);
  txt("dots", sheets > 1 ? sheet_dots(sheets, sheet_) : "");
  vis("dots", sheets > 1);
  // the whole group, at the top of every sheet
  CoverLook all;
  all.style = kCoverStyles[cover_style_index(str(page, "style"))];
  all.label = settings_.texts.cover_all;
  draw_cover_row(page, "all", str(page, "group"), all, true);
  for (const char *part : {"_text", "_up", "_stop", "_down", "_hs", "_up_hs", "_stop_hs", "_down_hs"})
    vis(std::string("all") + part, true);
  for (int r = 0; r < kCoverRowsPerSheet; r++) {
    const int i = sheet_ * per + r - first;
    const std::string n = "row" + std::to_string(r + 1);
    const bool on = r >= first && i < static_cast<int>(rows.size());
    if (on) {
      const CoverLook look = member_look(page, rows[i].second);
      const std::string item = rows[i].second.name;
      const auto draw = [this, page, n, item, look] { draw_cover_row(page, n, item, look, false); };
      bindings_.push_back({item, draw});
      draw();
    }
    for (const char *part : {"_text", "_value", "_up", "_stop", "_down", "_hs", "_up_hs", "_stop_hs", "_down_hs"})
      vis(n + part, on);
  }
  for (int k = 0; k < 3; k++)
    vis("preset" + std::to_string(k + 1) + "_hs", k < static_cast<int>(look.presets.size()));
  if (!look.presets.empty()) {
    const auto draw = [this, page] { draw_cover_presets(page); };
    bindings_.push_back({str(page, "group"), draw});   // a group with a state (AVG): the preset it is at
    draw();
  }
}

/* The group's presets: a pill each, its label on it; the one the group is at (a group with a state) or just
   tapped in the accent. */
void Engine::draw_cover_presets(const cJSON *page) {
  const auto pg = boxes_.find("covers");
  if (pg == boxes_.end())
    return;
  const CoverLook look = cover_look(page);
  const int closed = cover_settled(str(page, "group"), look.reverse);
  for (int k = 0; k < static_cast<int>(look.presets.size()); k++) {
    const std::string n = "preset" + std::to_string(k + 1);
    if (!pg->second.count(n + "_hs"))
      continue;
    const bool at = (closed >= 0 && std::abs(closed - look.presets[k].second) <= 1) ||
                    (preset_tapped_.live(now_ms_) && preset_tapped_.value == k);
    const int colour = at ? theme_.accent : kDarkGrey, ink = at ? kWhite : kLightGrey;
    const Box &bx = pg->second.at(n + "_hs");
    const std::string label = shorten(2, look.presets[k].first, bx.w - 12, false);
    const std::string key = std::to_string(colour) + "|" + label;
    if (drawn_["covers_" + n] == key)
      continue;
    drawn_["covers_" + n] = key;
    pill(bx, colour, ink, label);
  }
}

/* A row: its card, the small drawing, the name, the position, the three buttons -- the card and the
   buttons once, the drawing when it changes. */
void Engine::draw_cover_row(const cJSON *page, const std::string &n, const std::string &item, const CoverLook &look,
                            bool all) {
  const auto pg = boxes_.find("covers");
  if (pg == boxes_.end() || !pg->second.count(n + "_hs"))
    return;
  if (drawn_[n] != "card") {   // the card and the buttons: once a page
    drawn_[n] = "card";
    rounded(pg->second.at(n + "_hs"), all ? 12 : 10, kDarkGrey);
    const int si = cover_style_index(look.style);
    const char *parts[] = {"_up", "_stop", "_down"};
    for (int k = 0; k < 3; k++) {
      if (pg->second.count(n + parts[k] + "_hs"))
        rounded(pg->second.at(n + parts[k] + "_hs"), all ? 10 : 8, kCoverButton);
      txt(n + parts[k], icon_glyph(cover_icon(si, k, look.from_left)));
      send("ref " + n + parts[k]);
    }
    txt(n + "_text", all ? settings_.texts.cover_all : look.label);
    send("ref " + n + "_text");
    if (!all)
      send("ref " + n + "_value");
  }
  const int closed = all ? -1 : cover_settled(item, look.reverse);
  if (!all)   // in %: the words ("heruntergelassen") would leave the name no room; the whole group has none
    txt(n + "_value", closed < 0 ? "" : std::to_string(closed) + " %");
  if (pg->second.count(n + "_mini")) {
    const Box &m = pg->second.at(n + "_mini");
    const std::string key = look.style + "|" + std::to_string(closed) + "|" + std::to_string(theme_.accent);
    if (drawn_[n + "_mini"] != key) {
      drawn_[n + "_mini"] = key;
      draw_cover_art(m.x, m.y, m.w, m.h, look.style, closed < 0 ? 0 : closed, kDarkGrey, look.from_left);
    }
  }
  (void)page;
}

void Engine::covers_event(const cJSON *page, const std::string &component) {
  const auto rows = cover_rows(page);
  if (component.rfind("preset", 0) == 0 && component.size() == 7) {   // a preset: its position to the whole group
    const CoverLook look = cover_look(page);
    const int k = component[6] - '1';
    if (k >= 0 && k < static_cast<int>(look.presets.size())) {
      cover_command(str(page, "group"), std::to_string(look.presets[k].second), look.reverse, member(page, "commands"));
      preset_tapped_.hold(k, now_ms_, 3000);   // lit at once, for a while (or until the group is there)
      draw_cover_presets(page);
    }
    return;
  }
  const bool all = component.rfind("all", 0) == 0;
  if (!all && (component.size() < 4 || component.rfind("row", 0) != 0))
    return;
  const int first = cover_look(page).presets.empty() ? 0 : 1, per = kCoverRowsPerSheet - first;
  const int r = all ? -1 : component[3] - '1', i = sheet_ * per + r - first;
  if (!all && (r < first || r >= kCoverRowsPerSheet || i >= static_cast<int>(rows.size())))
    return;
  const CoverLook look = all ? CoverLook{} : member_look(page, rows[i].second);
  const std::string item = all ? str(page, "group") : rows[i].second.name, part = component.substr(all ? 3 : 4);
  if (part == "_up" || part == "_stop" || part == "_down") {
    const std::string which = part == "_up" ? "open" : part == "_stop" ? "stop" : "close";
    const cJSON *cmds = member(page, "commands");
    std::vector<CoverLook> looks;   // the whole group, members with a command item of their own: each its own command
    for (size_t k = 0; all && k < rows.size(); k++)
      looks.push_back(member_look(page, rows[k].second));
    if (std::any_of(looks.begin(), looks.end(), [](const CoverLook &l) { return !l.command_item.empty(); })) {
      for (size_t k = 0; k < rows.size(); k++)
        cover_command(rows[k].first, which, looks[k].reverse, cmds, looks[k].command_item);
    } else {
      cover_command(item, which, look.reverse, cmds, look.command_item);
    }
    // the button's icon: in the accent (the touch made it so, on the display) for a while, then back
    const std::string icon = component;
    sent_.erase(icon + ".pco");
    num(icon, "pco", theme_.accent);
    button_lit_[icon] = now_ms_ + 1500;
    return;
  }
  if (part.empty() && !all) {   // the member as a cover page of its own, over this one
    cJSON_Delete(member_page_);
    member_page_ = cJSON_CreateObject();
    cJSON_AddStringToObject(member_page_, "id", "@cover");
    cJSON_AddStringToObject(member_page_, "type", "cover");
    cJSON_AddStringToObject(member_page_, "title", look.label.c_str());
    cJSON_AddStringToObject(member_page_, "item", item.c_str());
    cJSON_AddStringToObject(member_page_, "style", look.style.c_str());
    if (look.reverse)
      cJSON_AddBoolToObject(member_page_, "reverse", true);
    if (look.from_left)
      cJSON_AddStringToObject(member_page_, "from", "left");
    if (!look.moving.empty()) {
      cJSON *moving = cJSON_AddObjectToObject(member_page_, "moving");
      cJSON_AddStringToObject(moving, "item", look.moving.c_str());
    }
    if (const cJSON *cmds = member(page, "commands"); cmds || !look.command_item.empty()) {   // the group's words
      cJSON *own = cmds ? cJSON_Duplicate(cmds, true) : cJSON_CreateObject();
      cJSON_DeleteItemFromObject(own, "item");
      if (!look.command_item.empty())
        cJSON_AddStringToObject(own, "item", look.command_item.c_str());
      cJSON_AddItemToObject(member_page_, "commands", own);
    }
    cJSON *presets = cJSON_AddArrayToObject(member_page_, "presets");
    for (const auto &[label, pos] : look.presets) {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "label", label.c_str());
      cJSON_AddNumberToObject(e, "position", pos);
      cJSON_AddItemToArray(presets, e);
    }
    open("@cover");
  }
}



}  // namespace appdisplay
