/*
   The display logic: which page is shown, what is on it, what a touch does.

   Free of ESP-IDF -- the panel configuration, item states and display
   messages go in, display commands come out through a Sink -- so the whole
   behaviour runs in the host tests. Not thread-safe: one task owns it (see
   Display.h for the one on the device).

   The display design has one template per page type (home, buttons,
   entities -- one per style --, cover, light, media, weather, qr, system);
   every page of the configuration is drawn on its template. The
   design reports touches as events instead of switching pages itself, so
   navigation lives here: the configured swipe row, and a history for close.
*/

#pragma once

#include "appdisplay/Cover.h"
#include "appdisplay/Format.h"
#include "appdisplay/Logo.h"
#include "appoh/OpenHab.h"
#include "nextion/Protocol.h"

#include <cJSON.h>

#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace appdisplay {

/* Where display commands go (the UART on the device, a list in the tests). */
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void send(const std::string &command) = 0;
};

/* What the boot page and the home screen show about the panel itself. */
struct PanelStatus {
  std::string version;
  std::string device;
  std::string ip;
  std::string ipv6;      // the IPv6 address to show: a global one, else a unique local one ("" none)
  bool wifi = false;     // joined the home network
  bool hotspot = false;  // setup hotspot open
  bool setup = false;    // ... opened by hand: the setup page shows, whatever page was on
  std::string hotspot_ssid;
  bool openhab = false;  // state stream up
};

/* What the info page (page type "system") shows; the engine formats it. */
struct SystemInfo {
  std::string version;
  int64_t uptime_s = 0;
  std::string ip;
  std::string ssid;  // the network joined
  /* The network page: the IPv4 address's prefix and gateway, the IPv6 addresses (kind "global",
     "unique-local", "link-local"; appnet::ipv6), the IPv6 router, the DNS servers in the order asked, the
     first time server, the name the panel goes by. */
  int prefix = 0;
  std::string gateway, gateway6, time_server, name;
  std::vector<std::pair<std::string, std::string>> ipv6;   // kind, address
  std::vector<std::string> dns;
  int rssi = 0;  // dBm; 0: not connected
  std::string openhab;  // openHAB's host (and port), "" when not set up
  bool openhab_https = false;
  size_t heap_free = 0, heap_min = 0;
  size_t psram_free = 0, psram_total = 0;
  double temperature = NAN;  // degrees Celsius, corrected
  std::string display_model, display_firmware;
  std::string display_design;     // the version of the design it runs ("" unknown)
  bool display_outdated = false;  // the display runs another design than this firmware was built for
};

struct Callbacks {
  /* The last state of an item; false if none has arrived. */
  std::function<bool(const std::string &item, appoh::ItemState &out)> state;
  /* Sends a command to an openHAB item. */
  std::function<void(const std::string &item, const std::string &command)> command;
  /* ... one holding a code (the keypad page's): not logged (else: command). */
  std::function<void(const std::string &item, const std::string &command)> secret_command;
  /* Switches a relay (index 0/1) the panel controls directly. */
  std::function<void(int index, bool on)> relay;
  /* The restart button of the settings page. */
  std::function<void()> restart;
  /* The values of the info page, asked for while it is shown. */
  std::function<SystemInfo()> system;
  /* A display.screen number that came from its openHAB item, to keep in the
     stored configuration ("brightness", "idle_brightness", "idle_after"). */
  std::function<void(const std::string &key, int value)> remember;
  /* The options of an item (appoh::options): false while not known yet;
     `refresh`: asked of openHAB again (in the background). */
  std::function<bool(const std::string &item, std::vector<appoh::Option> &out, bool refresh)> options;
  /* The range of an item (appoh::limits: its stateDescription's minimum and
     maximum), from the same cache; false while not known or without one. */
  std::function<bool(const std::string &item, appoh::Limits &out)> limits;
  /* A group's members (appoh::members), from the same cache: false while not
     known; `refresh`: asked of openHAB again (in the background). */
  std::function<bool(const std::string &group, std::vector<appoh::Member> &out, bool refresh)> members;
  /* An item's own metadata, by namespace (appoh::metadata), from the same cache: false while not known;
     `refresh`: asked of openHAB again (in the background). */
  std::function<bool(const std::string &item, std::map<std::string, appoh::Metadata> &out, bool refresh)> metadata;
  /* The state an item will most likely have soon (appoh::expect). */
  std::function<void(const std::string &item, const std::string &state)> expect;
  /* Loads a cover picture (cover_source) as size x size pixels, each scale
     px large, corners rounded by radius px; on_cover tells when it came. */
  std::function<void(const std::string &source, int size, int scale, int radius, Reveal reveal)> cover;
};

class Engine {
 public:
  Engine(Sink &sink, Callbacks callbacks);
  ~Engine();

  /* The built design (boards/nspanel/nextion/display-manifest.json) and the weather icon
     sets (boards/nspanel/nextion/weather-icons.json). False if either does not parse. */
  bool load_design(const char *manifest_json, const char *weather_json);

  /* A new panel configuration (already validated). Redraws the current page. */
  void set_config(const std::string &json);

  void set_status(const PanelStatus &status);

  /* State of the relays the panel switches directly (no item configured). */
  void set_relay(int index, bool on);
  void draw_wifi();

  /* What the display sends (its touches, the pages it loaded) -- while the
     link is up (Display.cpp keeps its own startup messages from here). */
  void on_message(const nextion::Message &message);

  /* The link to the display (Display.cpp): up -- what the display shows is
     not known, the theme and the background are sent and the page drawn,
     once --; down -- nothing is sent until it is up again (the display
     restarts, is being rewritten or does not answer). Up at first. */
  void link_up();
  void link_down();
  bool link() const { return link_; }
  /* A configuration has come (set_config): what the display is to show, its theme first, is known. */
  bool configured() const { return config_ != nullptr; }

  /* What was known of what the display shows is forgotten: nothing is drawn
     on it until it reports a page again. */
  void display_away();

  /* Animated icons (an indicator with a list of icons) one frame on, when
     due; call often (every 50 ms) while animating() is true. */
  void animate(int64_t now_ms);

  /* What time without a touch does: back to home after display.home_after
     seconds on another page; the screen's brightness, dimmed after
     display.screen.idle_after, off at night (a black page a touch wakes
     from). `now_ms`: a monotonic clock (not the wall clock, which a time sync
     may move). */
  void idle(int64_t now_ms);
  bool asleep() const { return asleep_; }
  int brightness() const { return level_; }  // -1: not sent yet
  bool animating() const { return !anims_.empty(); }
  /* A touch's release waits a moment (release_wait) in case it ended a swipe:
     the display task wants idle() soon then. */
  bool pending() const {   // idle() soon, please
    return pending_.has_value() || !logo_queue_.empty() || cover_job_.has_value() || fade_.phase ||
           wheel_pending_.has_value();
  }
  /* The time of what comes next (ms, as idle() takes it): the display task
     sets it before each message, so events are timed when they come. */
  void clock(int64_t now_ms) { now_ms_ = now_ms; }
  /* How long a release waits for a swipe it may have ended (ms; 0: acts at
     once, swipe or not -- the host tests). */
  void release_wait(int ms) { release_wait_ms_ = ms; }

  /* A state arrived; redraws what shows it on the current page. */
  void on_state(const std::string &item);
  /* A cover asked for (Callbacks::cover) has been loaded; null: it could not be. */
  void on_cover(const std::string &source, std::shared_ptr<Cover> cover);
  /* The options of `item` have come (Callbacks::options) -- or a group's members: pages that list them redraw. */
  void on_options(const std::string &item);

  /* Redraws everything on the current page that shows a state (some states
     may have arrived unseen). */
  void redraw_states();

  /* Clock: call at least once a minute. */
  void tick(time_t now);

  /* Leaves the boot page for home once the panel is ready. */
  void start();

  /* A hardware button was pressed (index 0/1). */
  void hardware_button(int index);

  /* Configured id of the logical page shown ("home" or a page id). */
  const std::string &current() const { return current_; }

 private:
  struct Binding {
    std::string item;
    std::function<void()> draw;
  };
  struct PageInfo {
    int id = -1;
    bool background = false;
    std::vector<std::string> crop;
    bool close = false;                    // has a close cross (a picture per background)
  };

  Sink &sink_;
  Callbacks cb_;
  cJSON *config_ = nullptr;
  cJSON *weather_ = nullptr;
  DisplaySettings settings_;
  PanelStatus status_;
  bool relays_[2] = {false, false};
  bool relay_pending_[2] = {false, false};  // pressed while openHAB was away (see hardware_button)
  bool relay_told_[2] = {false, false};     // openHAB told the relay's state while its item was NULL (follow_relays)

  std::map<std::string, PageInfo> pages_;   // physical page name -> info
  std::map<std::string, int> backgrounds_;  // background id -> picture id
  // background id -> "slot/picture" -> picture id: icons and tiles per background
  std::map<std::string, std::map<std::string, int>> pictures_;
  std::string default_background_;
  /* Text fields shortened to fit (manifest "fit": page -> object -> font,
     width) and the advance widths of their fonts (manifest "fonts"). */
  struct FontWidths {
    uint32_t first = 32;
    std::vector<uint8_t> widths;
    std::vector<uint8_t> lead, trail;  // blank columns before and after the ink, inside the width
    /* The characters the lists hold, one run after the other (manifest "ranges": [first, count]):
       Latin, then e.g. Greek and Cyrillic -- not the blocks in between. Without it, one run from first. */
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    /* Where character `c` is in the lists; "?"'s place for one they do not hold. */
    uint32_t index(uint32_t c) const;
  };
  std::map<int, FontWidths> fonts_;
  struct Fit {
    int font = 0, width = 0;
    int maxl = 0;   // the field's room in bytes (txt_maxl): a longer text the display cuts silently; 0: unknown
  };
  std::map<std::string, std::map<std::string, Fit>> fit_;
  struct Box {
    int x, y, w, h;
  };
  std::map<std::string, std::map<std::string, Box>> boxes_;   // page -> field -> where the firmware draws (a logo, the QR code)
  std::string fitted(const std::string &page, const std::string &obj, const std::string &text) const;
  static std::string within_bytes(const std::string &text, int maxl);
  int text_width(int font, const std::string &text) const;
  /* Whether `text` fits field `obj` of page `page` (its font and width, the manifest's "fit"). */
  bool fits(const std::string &page, const std::string &obj, const std::string &text) const;
  /* `text` in two lines for fields `first` and `second` of `page`: broken between words, within a word only where
     one does not fit the first; the second is cut short by txt() where it is too long. */
  std::pair<std::string, std::string> two_lines(const std::string &page, const std::string &first,
                                                const std::string &text) const;
  /* A number as a large figure and its unit apart (rounded to the ref's
     "decimals", default 0); anything else whole in `number`. */
  void number_and_unit(const cJSON *ref, std::string &number, std::string &unit,
                       const RuleStyle *style = nullptr) const;
  /* The unit fields after a number (manifest "units"): prefix -> the number's
     x, a digit's width, how many places (prefix1 .. prefixN), its font. */
  struct UnitPlaces {
    int x = 0, step = 1, n = 0, font = 0;
    std::string field, numbers;  // the wide field, the narrow ones' prefix
  };
  std::map<std::string, UnitPlaces> units_;
  void show_number(const std::string &prefix, bool shown, const std::string &number = "",
                   const std::string &unit = "", int colour = -1, int blink = 0);

  /* The colours that go with the background (manifest "backgrounds"), with
     the configuration's display.theme on top. */
  struct Theme {
    int accent = 1055, text = 65535, dim = 52857;
  };
  std::map<std::string, Theme> bg_themes_;
  Theme theme_;
  void update_theme();
  void send_theme();
  void hide_busy();
  struct Animation {
    std::vector<std::string> frames;   // glyphs
    int interval_ms = 500;
    int64_t next_ms = -1;
    size_t at = 0;
  };
  std::map<std::string, Animation> anims_;   // object -> its frames, while shown
  void show_icon(const std::string &obj, const cJSON *ref, int colour);
  void show_frames(const std::string &obj, std::vector<std::string> frames, int interval_ms);
  void show_text(const std::string &obj, const std::string &text, int blink_ms);
  RuleStyle style_of(const cJSON *ref, const cJSON *rules = nullptr) const;
  const std::vector<std::string> &swipe_row() const { return swipe_row_; }
  std::string start_page() const { return swipe_row_.front(); }   // the row's first: where "back" ends
  int sheets_of(const cJSON *page);   // a list's, the weather's, a cover group's sheets (else 1)
  bool in_swipe_row() const;   // the page shown is in the swipe row (of more than one): its dots are the row's
  std::string sheet_dots(int sheets, int at) const;   // a dot a sheet, `at` full
  void make_swipe_row();   // swipe_row_ from the configuration (set_config)
  std::vector<std::string> swipe_row_{"home"};
  bool no_close_sent_ = false;   // what the display's no_close says (Program.s; the display starts with 0)
  bool refresh_paused_ = false;   // inside a ref_stop .. ref_star of our own
  static std::string degrees_only(const std::string &unit);
  void draw_range(const cJSON *home);
  struct { int step = 1, n = 0, font = 0; } range_;  // manifest "range"
  struct { int x0 = 0, step = 1, n = 0, font = 0, high_x = 0; } weather_line_;   // manifest "weather_line"
  // manifest "screen": the display as the design is turned, and what of it shows (the NSPanel EU's 450 px of 480)
  struct { int w = 480, h = 320, visible_w = 450, visible_h = 320; } display_size_;
  void follow_relays(const std::string *item);
  void page_colour(const std::string &obj, int value);
  std::string theme_sent_;  // what the display's th_* variables hold
  int bg_sent_ = -1;                             // the background picture last sent (bg_pic)

  bool started_ = false;
  std::string current_ = "home";           // logical page
  int64_t now_ms_ = 0, touched_ms_ = -1;   // idle(): the clock, the last touch (-1: none yet)
  int64_t start_ms_ = 0;                   // the last time on the start page (home_after counts from leaving it)
  ScreenSettings screen_;                  // display.screen, with what its items say
  bool screen_known_ = false;              // a configuration was read (set_config) before
  bool asleep_ = false;                    // the black page of the night is shown
  int level_ = -1;                         // the brightness sent (dim=), -1: unknown
  std::map<std::string, int> remember_;    // screen numbers from items or the settings page, kept once they settle
  std::map<std::string, int> command_;     // ... and those of the settings page sent to their items then
  int64_t remember_at_ = 0;
  void screen_tick();
  void draw_settings(const cJSON *page);
  void settings_value(int slider, int value);  // a slider of the settings page moved
  int64_t restart_armed_ = 0;                  // the restart button pressed once (until then + 5 s)
  cJSON *settings_page_ = nullptr;             // the settings page, built in (id "settings")
  cJSON *info_page_ = nullptr;                 // the info page, built in (id "info", type "system")
  void screen_item(const std::string &item);
  void screen_config(const ScreenSettings &before);
  bool night_now() const;
  void set_level(int level);
  void show_current();
  std::vector<std::string> history_;       // for close
  std::string expected_;                   // physical page asked for, waiting for its load event
  std::string shown_;                      // physical page on screen
  bool building_buttons_ = false;          // draw_buttons shows the buttons itself, all at once
  bool bar_[8] = {};                       // the tiles with a bar (a value in %), for draw_buttons to show
  int sheet_ = 0;                          // entity pages: which eight rows
  int dots_max_ = 8;                       // how many page dots fit in their field (manifest "dots")
  time_t now_ = 0;
  bool dated_rules_ = false;   // the configuration has rules by a date (within_days): redrawn as the time goes on

  std::vector<Binding> bindings_;
  std::map<std::string, std::string> sent_;  // "obj.attr" -> last value, against flicker

  /* A notification (display.notifications.item): a card drawn over the page
     shown, which stands still under it (send() holds back what would draw on
     it); a touch anywhere closes it and the page is loaded again. Only a new
     state shows one -- not the one the item has when the panel starts. */
  struct Note {
    Notification what;
    int colour = 0;
  };
  std::optional<Note> note_;
  bool note_drawn_ = false;
  std::string notify_item_;
  bool notify_known_ = false;  // the item's state was seen once
  std::string notify_last_;
  void notify_state();
  void draw_note();
  /* Drawing on the screen, as the firmware draws its own shapes (lost when the page is drawn again). `over_note`:
     sent while a notification card is up too (the card's own drawing). */
  void fill(int x, int y, int w, int h, int colour, bool over_note = false);
  void disc(int x, int y, int r, int colour, bool over_note = false);
  void rounded(int x, int y, int w, int h, int r, int colour, bool over_note = false);   // corners of radius r
  void rounded(const Box &b, int r, int colour) { rounded(b.x, b.y, b.w, b.h, r, colour); }
  // round ends, its label on it (font 2), an icon before it
  void pill(const Box &b, int colour, int ink, const std::string &label, const std::string &glyph = "");
  void pill_shape(const Box &b, int colour);   // its round shape alone
  void relay_label(const Box &b, const std::string &side, const std::string &text, int colour);   // upright
  void restore(int x, int y, int w, int h);   // the page's background back (bg_pic)
  void close_note();
  std::vector<std::string> wrap(int font, const std::string &text, int width) const;
  std::string shorten(int font, const std::string &text, int width, bool cut) const;

  // --- output
  void send(const std::string &command);
  void batch_refresh(const std::function<void()> &draw);   // what `draw` changes shown in one go
  int hold_refresh_ = 0;   // batch_refresh: 1 paused before the next command, 2 paused
  void txt(const std::string &obj, const std::string &text);
  void num(const std::string &obj, const char *attr, int value);
  void vis(const std::string &obj, bool visible);

  // --- model
  const cJSON *page_config(const std::string &id) const;
  std::string template_of(const cJSON *page) const;
  appoh::ItemState state_of(const cJSON *ref) const;
  bool has_state(const cJSON *ref) const;
  /* What a field shows for ref: its text, or its item's state formatted by
     its "format", "factor" and "decimals" -- those it lacks from `dflt` (an
     entities page for its group's rows); a matching rule's format (`style`)
     instead, with its own factor. */
  std::string value_of(const cJSON *ref, const cJSON *dflt = nullptr, const RuleStyle *style = nullptr) const;
  bool on(const cJSON *ref) const;
  std::string toggle_command(const cJSON *ref) const;
  int picture(const std::string &slot, const std::string &name) const;
  int weather_pic(const cJSON *ref, const char *slot) const;
  std::string weather_icon(const cJSON *ref) const;
  std::string glyph(const cJSON *obj, const char *key = "icon") const;
  void bind(const cJSON *ref, std::function<void()> draw);

  // --- navigation
  void open(const std::string &id, bool remember = true, bool last_sheet = false);   // last_sheet: a swipe back
  void back();
  void swipe(const std::string &direction);
  void show_physical(const std::string &name);
  void on_page_loaded(const std::string &name);
  void on_event(const std::string &page, const std::string &component, const std::string &value);
  void act(const std::string &page, const std::string &component, const std::string &value);
  /* A release seen, acted on unless a swipe comes with it: lifting the finger
     reports the swipe and the release of what is under it -- the swipe wins. */
  struct Pending {
    std::string page, component, value;
    int64_t at = 0;
  };
  std::optional<Pending> pending_;
  int64_t last_swipe_ms_ = -100000;
  int release_wait_ms_ = 60;
  /* Logos and covers are drawn a part at a time (idle()), with a rest between
     the parts: the display reports a touch only when it gets round to it, and
     a stream of drawing commands without a pause kept a tap waiting until all
     was drawn. A finger coming down (a press) rests the drawing a while longer,
     so its release -- and the page it opens -- come at once. */
  static constexpr int64_t kDrawRestMs = 40;
  static constexpr int64_t kPressRestMs = 800;
  int64_t draw_rest_until_ms_ = 0;
  // the fade of a swipe to another page (page_fade): out, the page, waiting for it, in
  struct Fade {
    int phase = 0;   // 0 none, 1 out, 2 waiting for the page, 3 in
    int step = 0;
    int64_t at = 0;
    std::string target;
    bool last_sheet = false;   // a swipe back: the page on its last sheet
  } fade_;
  int fade_step_ms_ = 0;   // display.page_fade over the steps, set as a fade starts
  void fade_to(const std::string &id, bool last_sheet = false);
  void fade_tick();
  int64_t volume_sent_ms_ = 0, light_sent_ms_ = 0;   // the last value sent while a slider moves

  // --- drawing
  void render();
  /* A row of the entity page: label and value columns, one wide field, or
     (the info page) a short label and the value right-aligned. */
  int rows_per_sheet(const cJSON *page) const;
  std::vector<const cJSON *> days_of(const cJSON *page) const;
  static constexpr int kWeatherChips = 6;   // the weather page's cards of an extra
  const cJSON *rows_of(const cJSON *page);
  void forget_group_rows();
  std::map<std::string, cJSON *> group_rows_;   // an entity page's group: its members as rows, by page id
  bool group_asked_ = false;                     // the page's group asked for again since it opened
  void draw_boot();
  void draw_home();
  void draw_clock();
  void draw_relays();
  void draw_buttons(const cJSON *page);
  void draw_button(const cJSON *page, int index);
  void draw_entities(const cJSON *page, const std::string &physical);
  // the cover page and the cover group page (draw_cover, draw_covers)
  struct CoverLook {
    std::string style = "shutter";   // shutter, screen, awning, curtain, garage, gate
    bool reverse = false;            // the item's 0 is closed (extended): what shows is turned round
    bool from_left = false;          // a gate coming in from the left (it opens to the left)
    std::vector<std::pair<std::string, int>> presets;   // label, position as shown
    std::string label;               // a member's name (its metadata's "label", else its label)
    std::string moving;              // a member's item saying UP / DOWN / STOP (its metadata's "moving")
    std::string command_item;        // where up / stop / down go ("commands": "item", metadata "command_item"); "": the item
  };
  CoverLook cover_look(const cJSON *page) const;
  CoverLook member_look(const cJSON *group_page, const appoh::Member &m) const;
  static int cover_style_index(const std::string &style);
  static std::string cover_look_key(const CoverLook &look);   // what of the look the page shows, to compare
  std::string cover_drawn_key_;                                // ... as the cover page was drawn
  std::string covers_drawn_key_;                               // ... as the cover group page was drawn (its presets)
  int cover_closed(const std::string &item, bool reverse) const;   // closed this far as shown, 0..100; -1 unknown
  /* ... steadied: a jump (more than 10 points at once -- a cover moves a few % a second) shows only once it has
     lasted kCoverSettleMs: a binding's echo of the command, a device's target before its real position (seen:
     MQTT echoes 65, the device says 0, then 65 again, then 2, 5, 8 ...); another value before then and the jump
     is gone. A cover that reports only where it stops shows it that much later. */
  int cover_settled(const std::string &item, bool reverse);
  struct Settle {
    int shown = -1;          // the position shown (the item's terms)
    int pending = -1;        // a jump to an end, not shown yet
    int64_t since_ms = 0;
  };
  std::map<std::string, Settle> settle_;
  void draw_cover_art(int x, int y, int w, int h, const std::string &style, int closed, int bg, bool from_left);
  void draw_cover(const cJSON *page);
  void cover_event(const cJSON *page, const std::string &component);
  void cover_command(const std::string &item, const std::string &which, bool reverse, const cJSON *commands,
                     const std::string &command_item = "");
  std::vector<std::pair<std::string, appoh::Member>> cover_rows(const cJSON *page);   // ("" all | member)
  void draw_covers(const cJSON *page);
  void draw_cover_presets(const cJSON *page);
  int cover_rows_per_sheet(const cJSON *page) const;
  void draw_cover_row(const cJSON *page, const std::string &name, const std::string &item, const CoverLook &look,
                      bool all);
  void covers_event(const cJSON *page, const std::string &component);
  void cover_tick();
  struct Motion {
    int last = -1;          // the position last seen
    int dir = 0;            // +1 to 100, -1 to 0 (the item's terms), 0 still
    int64_t until_ms = 0;   // moving until then (seen moving, or commanded)
    int64_t commanded_until_ms = 0;   // sent a way to go: the device's first reports (its old position) do not turn it
  };
  std::map<std::string, Motion> motion_;
  /* A value that holds for a while: a position the finger sets, a preset just tapped, a target just sent -- shown
     until the item says so, or the while has passed. */
  template <typename T>
  struct Timed {
    T value{};
    bool set = false;
    int64_t since_ms = -100000, until_ms = 0;
    void hold(T v, int64_t now, int64_t for_ms) {
      value = v;
      set = true;
      since_ms = now;
      until_ms = now + for_ms;
    }
    void clear() { set = false; }
    bool live(int64_t now) const { return set && now < until_ms; }
    bool due(int64_t now) const { return set && now >= until_ms; }
  };
  Timed<int> cover_drag_;          // the cover page: the position the finger is setting
  std::map<std::string, int64_t> button_lit_;   // the group page's buttons just tapped: lit until then
  Timed<int> preset_tapped_;       // ... a preset just tapped (lit until the cover is there)
  cJSON *member_page_ = nullptr;   // a member of a cover group page, opened as a cover page ("@cover")
  void draw_light(const cJSON *page);
  void light_refresh(const cJSON *page);
  void draw_wheel_mark(double hue, double saturation);
  void light_value(const std::string &name, int value);   // a slider of the light page, or its wheel
  void cover_value(const std::string &name, int value);   // the drawing touched ("cdrw") or let go ("cdru")
  // the thermostat page (page type "thermostat")
  struct ThermoRange {
    double min = 5, max = 30, step = 0.5;
  };
  ThermoRange thermo_range(const cJSON *page) const;
  std::vector<std::pair<std::string, std::string>> thermo_modes(const cJSON *page) const;   // label, value
  double thermo_target(const cJSON *page) const;   // as shown: the finger's, one just sent, else the item's (NaN)
  std::string thermo_text(const cJSON *page, double v) const;   // a target as written, with its unit
  bool thermo_locked(const cJSON *page) const;   // the mode's rules keep the target (no minus / plus, no dragging)
  bool thermo_off(const cJSON *page) const;      // its "switch" is off and "locked": the target not set, the ring grey
  void draw_thermostat(const cJSON *page);
  void thermo_refresh(const cJSON *page);
  void thermo_ring(const cJSON *page, double share);   // the beads filled as far as share (0..1; < 0: unknown)
  void thermo_event(const cJSON *page, const std::string &component);
  void thermo_value(const std::string &name, int value);   // the ring touched ("tdrw") or let go ("tdru")
  void thermo_send(const cJSON *page, double v);
  void thermo_tick();
  std::string thermo_key(const cJSON *page) const;   // the range and the modes: what the page was drawn for
  std::string thermo_drawn_key_;
  std::vector<int> thermo_beads_;    // the colour each bead was drawn in (-1: not drawn)
  int thermo_knob_ = -1;             // the bead the knob is on (-1: none drawn)
  Timed<double> thermo_drag_;        // the target the finger is setting
  bool thermo_touching_ = false;     // a touch began on the ring (not inside it)
  int thermo_from_ = 0;              // ... where: x * 1000 + y
  bool thermo_swiped_ = false;       // the last touch on the ring was a swipe: the target kept, the page changes
  Timed<double> thermo_sent_;        // a target sent: shown until the item says so, or a while has passed
  Timed<int> mode_tapped_;           // a mode just tapped: lit until the item says so, or a while has passed

  // the keypad page (page type "keypad"): the code is typed on the display, which sends it with a button's event
  struct KeypadButton {
    std::string label, icon, item, command, when;
    bool code = true;               // only with a code typed
    const cJSON *toggle = nullptr;  // "action": "toggle": its entry (item, "on") -- switched, lit while on
  };
  std::vector<KeypadButton> keypad_buttons(const cJSON *page) const;   // those that show in the state now, 4 at most
  void draw_keypad(const cJSON *page);
  void keypad_refresh(const cJSON *page);   // the state and the buttons
  void keypad_event(const cJSON *page, const std::string &component, const std::string &value);
  void keypad_message(const std::string &text, int colour, int64_t for_ms);   // over the code's places a while
  void keypad_clear(const cJSON *page);   // the code typed gone, its places empty
  void keypad_tick();
  int64_t keypad_key_ms_ = 0;          // a key last touched: the code goes after a while without one
  int64_t keypad_message_until_ = 0;   // a message shows until then
  Timed<int> keypad_tapped_;           // a button just pressed: lit until the state is its "when", or a while
  Timed<std::string> keypad_waiting_;  // the state a button sent for: not accepted when the state does not get there
  std::string keypad_result_;          // the result item's state when the code went (a new one: shown)
  int64_t keypad_result_until_ = 0;
  bool keypad_blink_on_ = true;        // the state blinking (a rule's "blink"): shown or not
  std::string keypad_shown_;           // the buttons that show (another set in another state: drawn anew)
  int64_t keypad_blink_ms_ = 0;
  struct ColourTemp {
    std::string unit;          // kelvin, mired, percent
    double min = 0, max = 100;
    bool warm_high = false;    // the item's high end is warm (mired; or "reverse")
    bool with_unit = false;    // its state comes with "K": so does a command
  };
  ColourTemp colour_temp(const cJSON *page) const;
  int temp_slider(const cJSON *page) const;
  std::pair<std::string, std::string> temp_at(const cJSON *page, int value) const;   // shown, command
  bool link_ = true;   // the link to the display is up (link_up / link_down)
  /* The boot page stays at least kBootPageMinMs once shown -- so what it says
     (openHAB connecting, connected) can be read --, even when the panel is
     ready sooner: home then follows at start_at_ms_. */
  static constexpr int64_t kBootPageMinMs = 3000;
  int64_t boot_shown_ms_ = -1;
  int64_t start_at_ms_ = 0;
  int light_view_ = 0;   // the light page's view: brightness, colour temperature, colour
  static constexpr int kRingSize = 24;           // the colour wheel's ring: its box (radius 11, wiped as a whole)
  int wheel_ring_x_ = -1, wheel_ring_y_ = -1;    // where it is drawn
  bool light_on(const cJSON *page) const;
  struct ColourAfterOn {                         // a colour picked while the light was off: sent once it is on
    std::string page;
    int hue = 0, sat = 0;
    int64_t until_ms = 0;
  };
  std::optional<ColourAfterOn> colour_after_on_;
  void light_power(const cJSON *page);
  void send_wheel(bool now);
  int64_t wheel_touched_ms_ = -100000, wheel_sent_ms_ = -100000;
  std::optional<std::pair<std::string, std::string>> wheel_pending_;   // item, "h,s,b" still to send
  void draw_media(const cJSON *page);
  // --- the media page and the favourites
  struct Station {
    std::string uri, label;
    std::string short_name, name;   // for its tile, as the player's title
    std::string icon, image;        // from the catalogue item's "ohpanel" metadata (image: a logo's code)
  };
  const Logo *logo_of(const std::string &code);   // a logo, null without (or for a broken code)
  void queue_logo(const std::string &field, const std::string &code, int under, const std::string &key);
  void draw_next_logo();
  std::map<std::string, Logo> logos_;            // code -> logo, decoded once
  std::map<std::string, std::string> drawn_;   // field -> what is drawn in it (key), until the page goes
  struct LogoJob {
    std::string page, field, key, code;
    int under;
    size_t at = 0;   // the next rectangle
  };
  bool draw_logo(LogoJob &job, const Logo &logo);
  std::vector<LogoJob> logo_queue_;              // logos to draw, a part per idle(): after the page has shown
  std::vector<Station> stations_of(const cJSON *page) const;  // the catalogue, empty while not known
  bool media_playing(const cJSON *page) const;
  const cJSON *favorites_of(const cJSON *media) const;
  void media_refresh(const cJSON *page);         // what changed with a state
  void media_event(const cJSON *page, const std::string &component);
  void draw_favorites(const cJSON *page);
  void favorites_event(const cJSON *page, int index);
  const cJSON *player_of(const cJSON *favorites) const;
  void prefetch_options();
  static constexpr int64_t kPrefetchDelayMs = 5000;
  int64_t prefetch_at_ = -1;                     // when to fetch the favourites' catalogues (-1: not due)
  bool favorites_asked_ = false;                 // the page's catalogue asked for again since it opened
  int station_sheet_ = 0;                        // the favourites: which eight
  struct Hint {                                  // a favourite chosen: its name until the player tells a title
    std::string page, name, track;               // the media page, the station, the title before
    std::string uri;                             // what played before (its whole address or title), then what it learnt
  };
  static std::string stream_key(const std::string &uri);
  std::map<std::string, std::string> stations_heard_;   // stream_key -> a favourite's name (learnt after a tap)
  std::optional<Hint> hint_;
  // the cover: asked for, loaded, being drawn (in parts, from idle())
  void draw_art(const cJSON *page);
  void draw_play(bool playing);
  static constexpr int kSliderOff = 0x632C;      // a light's brightness slider filled while it is off: grey (100, 100, 100)
  static constexpr int kSymbol = 0x10C3;         // the play/pause symbol on its disc: near black (20, 24, 28)
  void draw_art_step();
  std::string cover_asked_, cover_asked_reveal_;
  struct LoadedCover {
    std::string source, reveal;   // ... and the order its rectangles are in (display.cover_reveal)
    std::shared_ptr<Cover> picture;   // null: it could not be loaded
  };
  std::optional<LoadedCover> cover_;
  struct CoverJob {
    std::string source;
    size_t at = 0;   // the next rectangle
  };
  std::optional<CoverJob> cover_job_;
  // the volume page, over the media page that opened it
  int volume_shown_ = 0;                         // what it shows (set at once, before the echo)
  int64_t volume_moved_ms_ = -100000;            // when the finger last moved it
  /* The light's and the cover's slider: while the finger moves it -- and
     kSliderHoldMs after -- the states that come back are not shown on it (a
     lamp fading reports where it is on the way: the handle jumped back and
     forth); then it shows what the item says (slider_resync_, idle()) -- only
     where that is more than a step from where the finger left it (what the
     display shows: slider_left_), so letting go draws nothing anew. */
  static constexpr int64_t kSliderHoldMs = 2000;
  int64_t slider_moved_ms_ = -100000;
  bool slider_resync_ = false;
  int slider_left_ = -1;                         // where the finger left the slider (its val); -1: not known
  void slider_moved(const char *slider, int value);
  void slider_val(const char *slider, int value);
  bool slider_held() const { return now_ms_ - slider_moved_ms_ < kSliderHoldMs; }
  void cover_refresh(const cJSON *page);
  void draw_volume_page();
  void volume_event(const std::string &component);
  void volume_slider(bool done, int value);
  void draw_weather(const cJSON *page);
  void draw_qr(const cJSON *page);
  void draw_qr_code(const cJSON *page);
  void draw_code(const std::string &field, const std::string &text, const std::string &ecc, bool dots, int colour,
                 int pad);
  void draw_system(const cJSON *page);
  void draw_network_page();   // the info page's IP card, tapped: every address, gateway, DNS, time server
  void draw_dots();

  // --- actions
  void act_shortcut(int index);
  void act_button(const cJSON *page, int index);
};

}  // namespace appdisplay
