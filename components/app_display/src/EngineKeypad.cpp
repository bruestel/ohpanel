/*
   The keypad page (Engine::draw_keypad): a code typed on the display, sent with a button; the state above it.
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Icons.h"
#include "EngineDetail.h"

#include <algorithm>

namespace appdisplay {
using namespace detail;

// ------------------------------------------------------------ keypad page --

namespace {
constexpr int kIconFont = 7;               // the icon font at 24 px (ohpanel_hmi.py ICON_FONT)
constexpr int kLongest = 8;                // the code's places at most (ohpanel_hmi.py KEYPAD_LONGEST)
constexpr int kRefused = 63721;            // #f87171: not accepted
constexpr int64_t kSentMs = 2000;          // "sent" shows this long
constexpr int64_t kMessageMs = 3000;       // any other message
constexpr int64_t kWaitMs = 6000;          // the state has this long to become a button's "when"
constexpr int64_t kResultMs = 10000;       // ... the result item to change
constexpr int64_t kForgetMs = 20000;       // a code typed goes after this long without a key
}  // namespace

/* The page's buttons that show in the state now ("buttons": label, icon, item -- else the page's --, command --
   "{code}" the code typed --, code -- false: without one too --, when -- the state it is lit in --, show_when -- the
   states it shows in, one or a list; else always); 4 at most. */
std::vector<Engine::KeypadButton> Engine::keypad_buttons(const cJSON *page) const {
  std::vector<KeypadButton> out;
  const cJSON *state = member(page, "state");
  const std::string now = has_state(state) ? state_of(state).state : "";
  const cJSON *list = member(page, "buttons");
  for (const cJSON *e = cJSON_IsArray(list) ? list->child : nullptr; e && out.size() < 4; e = e->next) {
    if (const cJSON *show = member(e, "show_when")) {
      bool shown = cJSON_IsString(show) && now == show->valuestring;
      for (const cJSON *s = cJSON_IsArray(show) ? show->child : nullptr; s && !shown; s = s->next)
        shown = cJSON_IsString(s) && now == s->valuestring;
      if (!shown)
        continue;
    }
    KeypadButton b;
    b.label = str(e, "label");
    b.icon = str(e, "icon");
    b.item = str(e, "item", str(page, "item"));
    b.command = str(e, "command", "{code}");
    b.when = str(e, "when");
    b.code = flag(e, "code", true);
    out.push_back(b);
  }
  return out;
}

void Engine::draw_keypad(const cJSON *page) {
  txt("title", str(page, "title"));
  keypad_key_ms_ = 0;
  keypad_message_until_ = 0;
  keypad_tapped_.clear();
  keypad_waiting_.clear();
  keypad_result_until_ = 0;
  keypad_blink_on_ = true;
  keypad_blink_ms_ = now_ms_;
  // the code: as many places as it has (else one per character typed), as long as that at most; the bottom row
  // *, 0, # with "phone" (backspace then on the places)
  const cJSON *len = member(page, "length");
  const int places = cJSON_IsNumber(len) ? std::clamp(len->valueint, 1, kLongest) : 0;
  num("places", "val", places);
  num("longest", "val", places ? places : kLongest);
  const bool phone = str(page, "keys") == "phone";
  num("phone", "val", phone ? 1 : 0);
  vis("k10", phone);
  vis("k10i", !phone);
  txt("k12", phone ? "#" : "C");
  vis("back", phone);
  vis("back_hs", phone);
  keypad_clear(page);
  // the buttons' touch areas: the set for as many as show (the pills: keypad_refresh)
  keypad_shown_.clear();   // keypad_refresh shows the set there is

  bind(page, [this, page] { keypad_refresh(page); });
  for (const char *ref : {"state", "result"})
    if (const std::string item = str(member(page, ref), "item"); !item.empty())
      bindings_.push_back({item, [this, page] { keypad_refresh(page); }});
}

void Engine::keypad_clear(const cJSON *page) {
  const cJSON *len = member(page, "length");
  const int places = cJSON_IsNumber(len) ? std::clamp(len->valueint, 1, kLongest) : 0;
  std::string dots;
  for (int k = 0; k < places; k++)
    dots += icon_glyph("ohpanel-code-empty");
  send("code.txt=\"\"");
  send("typed.txt=\"" + nextion::quote(dots) + "\"");   // the display writes them itself as keys are pressed
  sent_.erase("typed.txt");
  if (str(page, "keys") == "phone")
    send("ref back");   // the field painted its whole box: backspace on it again
}

/* The state -- its rules' text, icon and colour, else the page's "text" and "icon" -- centred over the code, and the
   buttons as pills, an icon before the label; the one the state is in (or just pressed) in the accent. */
void Engine::keypad_refresh(const cJSON *page) {
  const auto pg = boxes_.find("keypad");
  if (pg == boxes_.end())
    return;
  const cJSON *state = member(page, "state");
  const bool stated = !str(state, "item").empty();
  const RuleStyle style = stated ? style_of(state) : RuleStyle{};
  std::string word = stated ? value_of(state, nullptr, &style) : str(page, "text");
  std::string icon = !style.icon.empty() ? style.icon : str(stated ? state : page, "icon", str(page, "icon"));
  const int colour = style.color >= 0 ? style.color : color_of(stated ? state : page, "color", theme_.text);
  const bool shown = style.blink <= 0 || keypad_blink_on_;
  if (pg->second.count("state_hs")) {
    const Box &b = pg->second.at("state_hs");
    const std::string key = std::to_string(colour) + "|" + icon + "|" + word + "|" + std::to_string(shown);
    if (drawn_["state"] != key) {
      drawn_["state"] = key;
      restore(b.x, b.y, b.w, b.h);
      if (shown && (!word.empty() || !icon.empty())) {
        const int icon_w = icon.empty() ? 0 : 24, gap = icon.empty() || word.empty() ? 0 : 6;
        word = shorten(2, word, b.w - icon_w - gap, false);
        const int tw = text_width(2, word);
        const int x = b.x + (b.w - (icon_w + gap + tw)) / 2;
        if (!icon.empty())
          send("xstr " + std::to_string(x) + "," + std::to_string(b.y) + "," + std::to_string(icon_w) + "," +
               std::to_string(b.h) + "," + std::to_string(kIconFont) + "," + std::to_string(colour) + ",0,1,1,3,\"" +
               nextion::quote(icon_glyph(icon.c_str())) + "\"");
        if (!word.empty())
          send("xstr " + std::to_string(x + icon_w + gap) + "," + std::to_string(b.y) + "," + std::to_string(tw + 2) +
               "," + std::to_string(b.h) + ",2," + std::to_string(colour) + ",0,0,1,3,\"" + nextion::quote(word) + "\"");
      }
    }
  }
  const std::string now = stated && has_state(state) ? state_of(state).state : "";
  const auto buttons = keypad_buttons(page);
  // the set that shows in this state ("show_when"): another one -- the old touch areas away, the buttons' place back
  // to the background, the new touch areas (the pills below)
  std::string set = std::to_string(buttons.size());
  for (const auto &b : buttons)
    set += "\n" + b.label + "|" + b.command;
  if (set != keypad_shown_) {
    const size_t was = keypad_shown_.empty() ? 0 : std::stoul(keypad_shown_);
    if (!keypad_shown_.empty()) {
      int x0 = 10000, y0 = 10000, x1 = 0, y1 = 0;
      for (const auto &[name, b] : pg->second)
        if (name.size() == 6 && name[0] == 'b' && name.compare(3, 3, "_hs") == 0) {
          x0 = std::min(x0, b.x), y0 = std::min(y0, b.y);
          x1 = std::max(x1, b.x + b.w), y1 = std::max(y1, b.y + b.h);
        }
      for (size_t k = 1; k <= was; k++)
        vis("b" + std::to_string(was) + std::to_string(k) + "_hs", false);
      if (x1 > x0)
        restore(x0, y0, x1 - x0, y1 - y0);
    }
    for (size_t k = 1; k <= buttons.size(); k++)
      vis("b" + std::to_string(buttons.size()) + std::to_string(k) + "_hs", true);
    for (size_t n = 1; n <= 4; n++)
      for (size_t k = 1; k <= n; k++)
        drawn_.erase("b" + std::to_string(n) + std::to_string(k));
    keypad_shown_ = set;
    keypad_tapped_.clear();   // its place is another button's now
  }
  for (size_t k = 0; k < buttons.size(); k++) {
    const std::string n = "b" + std::to_string(buttons.size()) + std::to_string(k + 1);
    if (!pg->second.count(n + "_hs"))
      continue;
    const KeypadButton &btn = buttons[k];
    const bool lit = keypad_tapped_.live(now_ms_) ? keypad_tapped_.value == static_cast<int>(k)
                                                  : !btn.when.empty() && now == btn.when;
    const int fill_colour = lit ? theme_.accent : kDarkGrey, ink = lit ? kWhite : kLightGrey;
    const Box &bx = pg->second.at(n + "_hs");
    // the label in font 2 (font 1 on the small pills), else smaller; the icon left out where it would not fit
    int font = bx.h >= 44 ? 2 : 1;
    std::string glyph = btn.icon.empty() ? "" : icon_glyph(btn.icon.c_str());
    const int room = bx.w - 16;
    if (font == 2 && text_width(2, btn.label) + (glyph.empty() ? 0 : 30) > room)
      font = 1;
    if (!glyph.empty() && text_width(font, btn.label) + 30 > room)
      glyph.clear();
    const int icon_w = glyph.empty() ? 0 : 24, gap = glyph.empty() ? 0 : 6;
    const std::string label = shorten(font, btn.label, room - icon_w - gap, false);
    const std::string key = std::to_string(fill_colour) + "|" + glyph + "|" + label;
    if (drawn_[n] == key)
      continue;
    drawn_[n] = key;
    // a pill (Engine::pill), the icon and the label on it, centred together
    const int r = (bx.h - 1) / 2, top = bx.y + (bx.h - (2 * r + 1)) / 2;
    fill(bx.x + r, top, bx.w - 2 * r, 2 * r + 1, fill_colour);
    disc(bx.x + r, top + r, r, fill_colour);
    disc(bx.x + bx.w - 1 - r, top + r, r, fill_colour);
    const int tw = text_width(font, label), x = bx.x + (bx.w - (icon_w + gap + tw)) / 2;
    if (!glyph.empty())
      send("xstr " + std::to_string(x) + "," + std::to_string(top) + "," + std::to_string(icon_w) + "," +
           std::to_string(2 * r + 1) + "," + std::to_string(kIconFont) + "," + std::to_string(ink) + ",0,1,1,3,\"" +
           nextion::quote(glyph) + "\"");
    send("xstr " + std::to_string(x + icon_w + gap) + "," + std::to_string(top) + "," + std::to_string(tw + 2) + "," +
         std::to_string(2 * r + 1) + "," + std::to_string(font) + "," + std::to_string(ink) + ",0,0,1,3,\"" +
         nextion::quote(label) + "\"");
  }
}

/* A message where the code's places are, for a while: the display hides it again itself as soon as a key is pressed
   (the places then). */
void Engine::keypad_message(const std::string &text, int colour, int64_t for_ms) {
  txt("message", text);
  num("message", "pco", colour);
  send("vis typed,0");
  send("vis message,1");
  sent_.erase("message.txt");   // the display changes what shows there itself
  keypad_message_until_ = now_ms_ + for_ms;
}

/* A key touched ("key"), or a button let go ("button<k>", the code typed as its value): the code to the button's
   item in its command; a button wanting a code says so instead when none was typed. */
void Engine::keypad_event(const cJSON *page, const std::string &component, const std::string &value) {
  const Texts &t = settings_.texts;
  if (component == "key") {
    keypad_key_ms_ = now_ms_;
    return;
  }
  if (component.rfind("button", 0) != 0 || component.size() != 7)
    return;
  const int k = component[6] - '1';
  const auto buttons = keypad_buttons(page);
  if (k < 0 || k >= static_cast<int>(buttons.size()))
    return;
  keypad_key_ms_ = 0;   // the display has emptied it
  const KeypadButton &b = buttons[k];
  if (b.code && value.empty()) {
    keypad_message(t.keypad_enter, theme_.text, kMessageMs);
    return;
  }
  std::string command = b.command;
  for (size_t at = command.find("{code}"); at != std::string::npos; at = command.find("{code}", at + value.size()))
    command.replace(at, 6, value);
  const auto &send_it = cb_.secret_command ? cb_.secret_command : cb_.command;   // a code: not in the log
  if (b.item.empty() || command.empty() || !send_it)
    return;
  send_it(b.item, command);
  keypad_tapped_.hold(k, now_ms_, kWaitMs);   // lit at once, until the state says so (keypad_tick)
  if (!b.when.empty() && !str(member(page, "state"), "item").empty())
    keypad_waiting_.hold(b.when, now_ms_, kWaitMs);
  if (const cJSON *result = member(page, "result"); !str(result, "item").empty()) {
    keypad_result_ = has_state(result) ? state_of(result).state : "";
    keypad_result_until_ = now_ms_ + kResultMs;
  }
  keypad_message(t.keypad_sent, theme_.dim, kSentMs);
  keypad_refresh(page);
}

/* The message's end; a button's "when" reached, or not in time (not accepted); the result item's new state; the
   code typed forgotten after a while; the state blinking. */
void Engine::keypad_tick() {
  const cJSON *page = current_ == "home" ? nullptr : page_config(current_);
  if (!page || shown_ != "keypad")
    return;
  const Texts &t = settings_.texts;
  bool again = false;
  const cJSON *state = member(page, "state");
  const std::string now = has_state(state) ? state_of(state).state : "";
  const auto buttons = keypad_buttons(page);
  const auto when = [&](int k) { return k >= 0 && k < static_cast<int>(buttons.size()) ? buttons[k].when : ""; };
  if (keypad_waiting_.set) {
    if (!now.empty() && now == keypad_waiting_.value) {
      keypad_waiting_.clear();
    } else if (keypad_waiting_.due(now_ms_)) {
      keypad_waiting_.clear();
      keypad_message(t.keypad_refused, kRefused, kMessageMs);
    }
  }
  if (keypad_tapped_.set && (keypad_tapped_.due(now_ms_) || (!now.empty() && now == when(keypad_tapped_.value)))) {
    keypad_tapped_.clear();
    again = true;
  }
  if (keypad_result_until_) {
    const cJSON *result = member(page, "result");
    const std::string s = has_state(result) ? state_of(result).state : "";
    if (now_ms_ >= keypad_result_until_) {
      keypad_result_until_ = 0;
    } else if (s != keypad_result_) {
      keypad_result_until_ = 0;
      keypad_waiting_.clear();   // what it says instead -- the button no longer lit either
      if (keypad_tapped_.set) {
        keypad_tapped_.clear();
        again = true;
      }
      const RuleStyle style = style_of(result);
      keypad_message(value_of(result, nullptr, &style), style.color >= 0 ? style.color : theme_.text, kMessageMs);
    }
  }
  if (keypad_message_until_ && now_ms_ >= keypad_message_until_) {
    keypad_message_until_ = 0;
    send("vis message,0");
    send("vis typed,1");
    if (str(page, "keys") == "phone")
      send("ref back");
  }
  if (keypad_key_ms_ && now_ms_ - keypad_key_ms_ >= kForgetMs) {
    keypad_key_ms_ = 0;
    keypad_clear(page);
  }
  if (const RuleStyle style = str(member(page, "state"), "item").empty() ? RuleStyle{} : style_of(member(page, "state"));
      style.blink > 0 && now_ms_ - keypad_blink_ms_ >= std::clamp(style.blink, 200, 2000)) {
    keypad_blink_ms_ = now_ms_;
    keypad_blink_on_ = !keypad_blink_on_;
    again = true;
  }
  if (again)
    keypad_refresh(page);
}

}  // namespace appdisplay
