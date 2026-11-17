/*
   A tile's confirmation ("confirm" on a button, or on its button page for all of its tiles): a tap shows a question
   with two buttons before the tile switches. "confirm": true is the standard one -- the tile's label as the title,
   "Switch on?" / "Switch off?" / "Carry this out?" in the display language, "Yes" and "Cancel" --, an object one's
   own ({"title", "text", "icon", "icon_color", "yes": {"label", "color"}, "no": {"label"}, "timeout"}, each
   optional; "{label}" in its texts is the tile's label, "{to}" what it switches to).

   Drawn on the QR page's design (as the image page): its title, the rest drawn -- an icon, the question, two pills --
   over the tile's page, which comes back as it closes. Touches come as points (sendxy, as the notification card's):
   "Yes" switches, "Cancel", the close cross or "timeout" seconds (30) do nothing; a page opened meanwhile, the screen
   going off or a new configuration drop it.
*/

#include "appdisplay/Engine.h"

#include "appdisplay/Icons.h"
#include "EngineDetail.h"

#include <algorithm>

namespace appdisplay {

using namespace detail;

namespace {

constexpr int kConfirmTimeoutS = 30;
constexpr int kMargin = 16;       // the buttons from the edges and the bottom
constexpr int kButtonH = 52;
constexpr int kButtonMaxW = 180;
constexpr int kGap = 16;          // between the buttons
constexpr int kIconFontXl = 9;    // the icon font at 48 px (ohpanel_hmi.py ICON_FONT_XL)
constexpr int kIconH = 56;
constexpr int kTextFont = 3, kLineH = 28;
constexpr int kCloseW = 70;       // the close cross: the top right corner above the drawn area

std::string fill_in(std::string s, const std::string &label, const std::string &to) {
  for (const auto &[key, value] : {std::pair<std::string, std::string>{"{label}", label}, {"{to}", to}})
    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size()))
      s.replace(at, key.size(), value);
  return s;
}

}  // namespace

/* A tile about to send `command` to `item`: asked first when it (or its page) has "confirm" -- true then, the
   question shown; false: nothing in the way, send it. */
bool Engine::ask_confirm(const cJSON *page, const cJSON *button, const std::string &item, const std::string &command) {
  const cJSON *c = member(button, "confirm");
  if (!c)
    c = member(page, "confirm");
  if (!c || cJSON_IsFalse(c))
    return false;
  Confirm q;
  if (cJSON_IsObject(c))   // its own texts: a copy (a group's tiles are made anew while it shows)
    q.own = std::shared_ptr<cJSON>(cJSON_Duplicate(c, true), cJSON_Delete);
  else if (!cJSON_IsTrue(c))
    return false;
  q.item = item;
  q.command = command;
  q.label = str(button, "label", str(page, "title").c_str());
  q.since_ms = now_ms_;
  confirm_ = q;
  bindings_.clear();   // the tiles' fields are not on the question's page: drawn again as it closes
  show_physical("qr");
  return true;
}

void Engine::draw_confirm() {
  Confirm &c = *confirm_;
  const cJSON *own = c.own.get();
  const Texts &t = settings_.texts;
  const bool on = c.command == "ON", off = c.command == "OFF";
  const std::string to = on ? t.on : off ? t.off : c.command;
  const std::string title = fill_in(str(own, "title", c.label.c_str()), c.label, to);
  const std::string text = member(own, "text") ? fill_in(str(own, "text"), c.label, to)
                           : on ? t.confirm_on : off ? t.confirm_off : t.confirm_do;
  const std::string icon = str(own, "icon");
  const int icon_colour = color_of(own, "icon_color", theme_.text);
  const cJSON *yes = member(own, "yes"), *no = member(own, "no");
  const std::string yes_label = str(yes, "label", t.confirm_yes.c_str()), no_label = str(no, "label", t.confirm_no.c_str());
  const int yes_colour = color_of(yes, "color", theme_.accent);

  // the page's own objects where the question goes: away (the captions, the page dots, the busy indicator and its
  // timer -- a redraw of theirs would cover it)
  send("busy_tm.en=0");
  hide_busy();
  for (const char *o : {"caption1", "caption2", "dots"})
    vis(o, false);
  txt("title", title);
  // the text on the page's background as it is there (cropped from its picture): smooth edges, as the titles have
  const std::string ground = bg_sent_ >= 0 ? std::to_string(bg_sent_) + ",1,1,0" : "0,1,1,3";
  const int vw = display_size_.visible_w, top = image_area().y;
  const int bw = std::min(kButtonMaxW, (vw - 2 * kMargin - kGap) / 2), by = display_size_.h - kMargin - kButtonH;
  c.no = {vw / 2 - kGap / 2 - bw, by, bw, kButtonH};
  c.yes = {vw / 2 + kGap / 2, by, bw, kButtonH};
  // the icon and the question, together in the middle of what is above the buttons
  const int text_w = vw - 40;
  std::vector<std::string> lines = wrap(kTextFont, text, text_w);
  const int icon_h = icon.empty() ? 0 : kIconH;
  const size_t room = static_cast<size_t>(std::max(1, (by - 12 - top - icon_h) / kLineH));
  const bool more = lines.size() > room;
  if (more)
    lines.resize(room);
  for (size_t i = 0; i < lines.size(); i++)
    lines[i] = shorten(kTextFont, lines[i], text_w, more && i + 1 == lines.size());
  int y = top + std::max(0, (by - 12 - top - icon_h - static_cast<int>(lines.size()) * kLineH) / 2);
  if (!icon.empty()) {
    send("xstr " + std::to_string(vw / 2 - 32) + "," + std::to_string(y - 4) + ",64," + std::to_string(kIconH) + "," +
         std::to_string(kIconFontXl) + "," + std::to_string(icon_colour) + "," + ground + ",\"" +
         nextion::quote(icon_glyph(icon.c_str())) + "\"");
    y += icon_h;
  }
  for (const std::string &l : lines) {
    send("xstr 20," + std::to_string(y) + "," + std::to_string(text_w) + "," + std::to_string(kLineH + 2) + "," +
         std::to_string(kTextFont) + "," + std::to_string(theme_.text) + "," + ground + ",\"" + nextion::quote(l) + "\"");
    y += kLineH;
  }
  // the two buttons: pills, the label on each (font 2, else 1 where it does not fit) -- on the pill's colour, in its
  // straight middle (the round ends stay as drawn): smooth edges
  const auto button = [&](const Box &b, int fill_colour, int ink, const std::string &label) {
    pill_shape(b, fill_colour);
    const int inner = b.w - b.h;
    const int font = text_width(2, label) <= inner - 4 ? 2 : 1;
    send("xstr " + std::to_string(b.x + b.h / 2) + "," + std::to_string(b.y + 2) + "," + std::to_string(inner) + "," +
         std::to_string(b.h - 4) + "," + std::to_string(font) + "," + std::to_string(ink) + "," +
         std::to_string(fill_colour) + ",1,1,1,\"" + nextion::quote(shorten(font, label, inner - 4, false)) + "\"");
  };
  button(c.no, theme_.tile, theme_.tile_text, no_label);
  button(c.yes, yes_colour, kWhite, yes_label);
  send("tsw 255,0");   // the page's objects take no touch: points are reported instead (confirm_touch)
  send("sendxy=1");
  c.drawn = true;
}

/* A finger lifted at x, y on the question. */
void Engine::confirm_touch(int x, int y) {
  const Confirm &c = *confirm_;
  const auto in = [x, y](const Box &b) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; };
  const bool cross = y < image_area().y && x >= display_size_.visible_w - kCloseW;
  if (!in(c.yes) && !in(c.no) && !cross)
    return;
  sound(settings_.sound.touch, screen_.touch_volume);
  close_confirm(in(c.yes));
}

/* The question answered (`yes`: the tile's command sent) or let go: the tile's page again. */
void Engine::close_confirm(bool yes) {
  const Confirm c = *confirm_;
  drop_confirm();
  if (yes && cb_.command && !c.item.empty())
    cb_.command(c.item, c.command);
  show_current();
}

/* The question gone without an answer, without drawing anything (what drops it draws what comes). */
void Engine::drop_confirm() {
  if (!confirm_)
    return;
  if (confirm_->drawn && link_)
    sink_.send("sendxy=0");
  confirm_.reset();
}

/* No answer within the page's "timeout" -- counted while it shows: let go. */
void Engine::confirm_tick() {
  if (!confirm_)
    return;
  if (note_drawn_) {   // a notification over it: its time stands still (the whole of it again once the card goes)
    confirm_->since_ms = now_ms_;
    return;
  }
  const cJSON *own = confirm_->own.get();
  const int timeout = std::clamp(static_cast<int>(num_of(own, "timeout", kConfirmTimeoutS)), 5, 120);
  if (now_ms_ - confirm_->since_ms >= timeout * 1000LL)
    close_confirm(false);
}

}  // namespace appdisplay
