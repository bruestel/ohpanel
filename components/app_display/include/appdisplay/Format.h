/*
   How the panel writes values: display texts per language, date and time
   formats, colours and colour rules (docs/panel-config.md, "Display" and
   "value_style"). Free of ESP-IDF, so it is covered by the host tests.
*/

#pragma once

#include "appoh/OpenHab.h"

#include <cJSON.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <string>

namespace appdisplay {

/* The texts the panel writes itself. */
struct Texts {
  std::string on, off, undefined;
  std::array<std::string, 7> weekdays, weekdays_short;    // Sunday first
  std::array<std::string, 12> months, months_short;
  /* The info page (page type "system"): its title, then the row labels
     firmware, display, uptime, memory, PSRAM, WiFi, IP address, openHAB. */
  std::string system_title;
  std::array<std::string, 8> system;
  std::string design_outdated;  // the display row when the design does not match
  std::string free;             // memory left: "71 KB free"
  /* The settings page (page type "settings"): its title, the slider labels
     (brightness in use, when idle, idle after), "never" (idle after 0), the
     restart button and what it says once pressed. */
  std::string settings_title;
  std::array<std::string, 3> settings;
  std::string never, restart, restart_confirm;
  /* The boot page: its title while the panel starts (dots run after it) and in setup mode, the
     row labels device, WiFi and address (8 characters at most) and what
     openHAB is doing. */
  std::string boot_starting, boot_setup;
  std::string boot_device, boot_wifi, boot_address;
  std::string boot_connected, boot_connecting;
  /* The media page's volume card: its title and the mute button. */
  std::string volume_title, mute;
  /* The light page's views (its tiles): brightness, colour temperature, colour. */
  std::array<std::string, 3> light_views;
  /* The weather page's days without a title of their own: today, tomorrow (later ones: their weekday). */
  std::string today, tomorrow;
  /* The network page (the info page's IP card, tapped): its title and the row labels IPv4, gateway, IPv6
     (global), unique local, link-local, IPv6 router, DNS, time server, name. */
  std::string network_title;
  std::array<std::string, 9> network;
  /* The cover page's words per style (shutter, screen, awning, curtain, garage, gate): open, closed, opening,
     closing; and the cover group page's row for the whole group. */
  std::array<std::array<std::string, 4>, 6> cover_words;
  std::string cover_all;
  /* The thermostat page: before its target, and what it does (heating, cooling). */
  std::string thermostat_target, heating, cooling;
  /* The keypad page's messages: the code sent, not accepted (the state did not follow), a button wanting a code. */
  std::string keypad_sent, keypad_refused, keypad_enter;
};

/* display.screen: how bright the screen is, when it dims, when it is off.
   Each number may come from an openHAB item ("item"), which then wins. */
struct ScreenSettings {
  int brightness = 85;       // % while in use
  int idle_brightness = 60;  // % after idle_after s without a touch; 0: off
  int idle_after = 60;       // s; 0: never idle
  std::string brightness_item, idle_brightness_item, idle_after_item;
  // the night: off (as a black page, woken by a touch) -- from/to in minutes
  // of the day (-1: no window), or while the Switch item is ON
  int night_from = -1, night_to = -1;
  std::string night_item;
};

/* display.screen, resolved; a missing object yields the defaults. */
ScreenSettings screen_settings(const cJSON *screen);

/* Whether minute `now` of the day is in the night from..to (which may span
   midnight). */
bool in_night(int now, int from, int to);

/* display.* of the configuration, resolved. */
struct DisplaySettings {
  Texts texts;
  std::string date_format = "%a, %e %b %Y";
  std::string time_format = "%H:%M";
  std::string background;  // empty: the design's default
  char decimal = '.';      // the language's decimal separator
  char group = ',';        // ... and between thousands
  int home_after = 60;     // s without a touch on another page, then home; 0: never
  int page_fade = 0;       // ms a swipe to another page fades the screen out, and in again; 0: not
  std::string cover_reveal;   // how a cover appears (Cover.h reveal_of): "" (dissolve), "center", "sharpen", "wipe"
  ScreenSettings screen;
};

/* Reads display.* (language, texts overrides, formats); a missing or null
   object yields the defaults. */
DisplaySettings display_settings(const cJSON *display);

/* The strftime subset of docs/panel-config.md, with the language's names. */
std::string format_time(const std::string &format, const struct tm &t, const Texts &texts);

/* A notification of display.notifications.item: "level|title|text",
   "level|text", "title|text" or "text". The level is info, warning or alert
   (info when missing or unknown); a "\n" in the text breaks the line. */
struct Notification {
  std::string level = "info";
  std::string title, text;
};
Notification parse_notification(const std::string &value);

/* A colour of the configuration as RGB565: a palette name or #rrggbb. -1 if
   it is neither. */
int color565(const char *name);

/* The theme's colours (Engine::update_theme) that the placeholders "theme-accent", "theme-text" and "theme-dim"
   stand for wherever a colour is configured: they follow the theme. */
void set_theme_colors(int accent, int text, int dim);
/* The palette colour nearest to an RGB565 one, by name. */
std::string nearest_palette(int rgb565);

/* What a field shows for a state: "–" (texts.undefined) for NULL/UNDEF, a
   date/time for format "date"/"time"/"datetime" (Unix seconds in a Number, or
   a DateTime), any other format as a pattern (format_pattern; one without
   placeholders is a fixed text), without one openHAB's formatted value. `factor` multiplies a number first;
   other than 1 without a pattern, the number shows alone, with `decimals`. */
std::string format_state(const appoh::ItemState &s, const char *format, const DisplaySettings &d,
                         double factor = 1, int decimals = 0);

/* A pattern as openHAB's state patterns write it: %.1f a number with one
   decimal (%f: 6, %d: none, rounded), a "," after the % groups thousands
   (%,.0f), %s the state as it is, %unit% the item's unit, %% a percent sign;
   anything else stays. Separators as the language writes them. A %f or %d of
   a state that is no number shows openHAB's formatted value. */
std::string format_pattern(const std::string &pattern, const appoh::ItemState &s, const DisplaySettings &d,
                           double factor = 1);

/* The item's unit: of a Quantity, % of a Percent, else the one openHAB
   formats the number with ("13.1 °C" -> "°C"); "" without. */
std::string unit_of(const appoh::ItemState &s);

/* A number with `decimals` places, the decimal separator `decimal`, and with
   `thousands` the other one of "." and "," between groups of three. */
std::string format_number(double v, int decimals, bool thousands, char decimal, char group = 0);

/* A number as openHAB writes it (its displayState: a "." before the decimals,
   "," between thousands where the pattern asks for them -- whatever the
   server's locale) in the separators of the display language: each "." and
   "," between two digits swapped for `decimal` and the thousands separator
   that goes with it. Nothing else changes; no thousands are added. */
std::string localize_number(const std::string &text, char decimal, char group = 0);

/* Seconds since the epoch of a DateTime or Unix-seconds state; -1 if none. */
int64_t state_epoch(const appoh::ItemState &s);
/* "HH:MM" as minutes after midnight; -1 for anything else (nullptr too). */
int clock_minutes(const char *hh_mm);

/* What a value_style rule sets: a colour (RGB565, -1: none), blinking (ms
   on and off, 0: none), another icon (empty: none). */
struct RuleStyle {
  int color = -1;
  int blink = 0;
  std::string icon;
  int icon_color = -1;  // a value row's icon (its colour is the value's)
  std::string format;  // another format for the value, with its own factor
  double factor = 1;
  int active = -1;     // a button: drawn active (1) or not (0) while the rule matches; -1: as its state says
  std::string layout;  // a button: its icon "center" (large, alone) or "corner" while the rule matches; "": its own
  int locked = -1;     // a thermostat's mode: its target not to be set (1) while the rule matches
};

/* value_style rules: what the first matching rule sets; nothing if none
   matches. Days are compared in local time. A threshold "@key" is the
   number `vars` (an object: a group member's metadata) has under that key;
   a rule whose threshold is not there does not match. */
RuleStyle rule_style(const cJSON *rules, const appoh::ItemState &s, time_t now, const cJSON *vars = nullptr);

/* ON/OPEN/PLAY or a positive number. */
bool is_on(const appoh::ItemState &s);

/* The text of a QR code that joins a WPA network
   ("WIFI:T:WPA;S:<ssid>;P:<password>;;", with \ ; , : " escaped). */
std::string wifi_qr_text(const std::string &ssid, const std::string &password);

}  // namespace appdisplay
