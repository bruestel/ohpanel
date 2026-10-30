/*
   Host test for display formatting and the icon table.
*/

#include "appdisplay/Format.h"
#include "appdisplay/Icons.h"
#include "ntc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

using namespace appdisplay;

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                                   \
    }                                                                                 \
  } while (0)

appoh::ItemState st(const char *state, const char *type, double num = 0, bool has = false) {
  appoh::ItemState s;
  s.state = state;
  s.display = state;
  s.type = type;
  s.numeric = num;
  s.has_numeric = has;
  return s;
}
}  // namespace

int main(int argc, char **argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  // display settings and formats
  cJSON *d = cJSON_Parse(R"({"language":"de","date_format":"%a, %e. %b %y","texts":{"on":"Ein"}})");
  DisplaySettings ds = display_settings(d);
  cJSON_Delete(d);
  CHECK(ds.texts.on == "Ein" && ds.texts.off == "Aus");
  struct tm t = {};
  t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 5; t.tm_wday = 1; t.tm_hour = 13; t.tm_min = 35;
  CHECK(format_time(ds.date_format, t, ds.texts) == "Mo, 5. Okt. 26");
  DisplaySettings en = display_settings(nullptr);
  CHECK(format_time("%A, %d.%m.%Y %I:%M %p", t, en.texts) == "Monday, 05.10.2026 01:35 PM");
  CHECK(format_time("100%%", t, en.texts) == "100%");
  CHECK(format_time("%-I:%M %p", t, en.texts) == "1:35 PM");      // without the leading zero
  CHECK(format_time("%-H:%M %-d.%-m.", t, en.texts) == "13:35 5.10.");
  CHECK(format_time("%-q", t, en.texts) == "%-q");
  // the other languages: their words, separators and short date format (texts.json)
  {
    cJSON *f = cJSON_Parse(R"({"language":"fr"})");
    const DisplaySettings fr = display_settings(f);
    cJSON_Delete(f);
    CHECK(fr.texts.on == "Marche" && fr.texts.cover_words[2][0] == "rentré" && fr.texts.light_views[2] == "Couleur");
    CHECK(fr.decimal == ',' && fr.group == ' ' && format_time(fr.date_format, t, fr.texts) == "lun 5 oct");
    CHECK(format_state(appoh::ItemState{"12345.6 W", "12,345.6 W", "Quantity", "W", 12345.6, true}, "", fr) == "12 345,6 W");
    CHECK(format_number(1234567.5, 1, true, fr.decimal, fr.group) == "1 234 567,5");
    cJSON *g = cJSON_Parse(R"({"language":"fi"})");
    const DisplaySettings fi = display_settings(g);
    cJSON_Delete(g);
    CHECK(format_time(fi.date_format, t, fi.texts) == "ma 5.10." && fi.texts.today == "Tänään");
    // beyond Latin-1: Polish, Greek, Lithuanian (the display's text font has their letters)
    cJSON *p = cJSON_Parse(R"({"language":"pl"})");
    const DisplaySettings pl = display_settings(p);
    cJSON_Delete(p);
    CHECK(format_time(pl.date_format, t, pl.texts) == "pn 5 paź" && pl.texts.on == "Wł.");
    CHECK(format_number(1234567.5, 1, true, pl.decimal, pl.group) == "1 234 567,5");
    cJSON *e = cJSON_Parse(R"({"language":"el"})");
    const DisplaySettings el = display_settings(e);
    cJSON_Delete(e);
    CHECK(format_time(el.date_format, t, el.texts) == "Δευ 5 Οκτ");
    CHECK(format_number(1234567.5, 1, true, el.decimal, el.group) == "1.234.567,5");
    cJSON *li = cJSON_Parse(R"({"language":"lt"})");
    const DisplaySettings lt = display_settings(li);
    cJSON_Delete(li);
    CHECK(format_time(lt.date_format, t, lt.texts) == "Pr 10-05");
    cJSON *x = cJSON_Parse(R"({"language":"xx"})");
    const DisplaySettings unknown = display_settings(x);
    cJSON_Delete(x);
    CHECK(unknown.texts.on == "On" && unknown.decimal == '.');           // an unknown language: English
    CHECK(format_time(en.date_format, t, en.texts) == "Mon, 5 Oct");     // English's own, short
  }

  // colours
  CHECK(color565("red") == 63488 && color565("light-grey") == 52857);
  CHECK(color565("#ff0000") == 63488 && color565("#ffffff") == 65535);
  CHECK(color565("pink") == -1);

  // states
  CHECK(format_state(st("UNDEF", "UnDef"), nullptr, ds) == "–");
  appoh::ItemState dt = st("2026-10-05T12:33:59.045289479+0200", "DateTime");
  CHECK(state_epoch(dt) == 1791196439);  // 10:33:59 UTC
  CHECK(format_state(dt, "time", ds) == "12:33");
  CHECK(format_state(st("1791196439", "Decimal", 1791196439, true), "date", ds) == "Mo, 5. Okt. 26");
  appoh::ItemState pct = st("80", "Percent", 80, true);
  pct.display = "80 %";
  CHECK(format_state(pct, nullptr, ds) == "80 %");

  // patterns (ds: German, en: English)
  {
    appoh::ItemState t = st("21.456", "Quantity", 21.456, true);
    t.unit = "°C";
    t.display = "21.5 °C";
    CHECK(format_state(t, "%.1f %unit%", ds) == "21,5 °C");
    CHECK(format_state(t, "%.1f %unit%", en) == "21.5 °C");
    CHECK(format_state(t, "Inside %d°", en) == "Inside 21°");
    appoh::ItemState w = st("12345.6", "Quantity", 12345.6, true);
    w.unit = "W";
    CHECK(format_state(w, "%,.0f W", ds) == "12.346 W");
    CHECK(format_state(w, "%,.1f W", en) == "12,345.6 W");
    CHECK(format_state(w, "%.2f kW", ds, 0.001) == "12,35 kW");
    CHECK(format_state(w, nullptr, ds, 0.001, 1) == "12,3");     // scaled without a pattern: the number alone
    CHECK(format_state(pct, "Battery %d%%", en) == "Battery 80%");
    CHECK(format_state(pct, "%d %unit%", en) == "80 %");
    CHECK(format_state(st("OPEN", "String"), "Door: %s", en) == "Door: OPEN");
    CHECK(format_state(st("OPEN", "String"), "%.1f", en) == "OPEN");  // no number: openHAB's text
    CHECK(format_state(t, "100% %q", en) == "100% %q");               // not placeholders: as they are
    CHECK(format_state(st("-0.04", "Decimal", -0.04, true), "%.1f", en) == "0.0");
    CHECK(format_number(-1234567.891, 2, true, ',') == "-1.234.567,89");
    CHECK(format_number(999, 0, true, '.') == "999");
    CHECK(format_state(st("UNDEF", "UnDef"), "%.1f", ds) == "–");
    // openHAB's text of a number in the display language's separators -- not a text's, no thousands added
    CHECK(format_state(st("1.39 kWh", "Quantity", 1.39, true), "", ds) == "1,39 kWh");
    CHECK(format_state(st("69.0 %", "Decimal", 69, true), "", ds) == "69,0 %");
    CHECK(format_state(st("12,345.6 W", "Quantity", 12345.6, true), "", ds) == "12.345,6 W");   // "%,.1f"
    CHECK(format_state(st("2222 W", "Quantity", 2222, true), "", ds) == "2222 W");
    CHECK(format_state(st("Version 1.2", "String"), "", ds) == "Version 1.2");
    CHECK(localize_number("v1.2 at 3.5 W", '.') == "v1.2 at 3.5 W");
    CHECK(localize_number("-0.5 W. Done.", ',') == "-0,5 W. Done.");
    CHECK(format_state(st("1", "Decimal", 1, true), "On", ds) == "On");     // a fixed text
  }

  // the theme's colours by their placeholders: what the theme has now
  CHECK(color565("theme-accent") == -1);   // no theme yet
  set_theme_colors(1055, 65535, 33808);
  CHECK(color565("theme-accent") == 1055 && color565("theme-text") == 65535 && color565("theme-dim") == 33808);
  set_theme_colors(63488, 0, 0);
  CHECK(color565("theme-accent") == 63488);   // another theme: it follows

  // rules
  cJSON *rules = cJSON_Parse(R"([{"below":0,"color":"red"},{"color":"green"}])");
  CHECK(rule_style(rules, st("-5", "Quantity", -5, true), 0).color == 63488);
  CHECK(rule_style(rules, st("5", "Quantity", 5, true), 0).color == 21920);
  cJSON_Delete(rules);
  // a button's rules: active (or not) by a threshold; no rule matching: as its state says (-1)
  rules = cJSON_Parse(R"([{"above":65,"active":true,"color":"red"},{"below":30,"active":false}])");
  CHECK(rule_style(rules, st("70", "Quantity", 70, true), 0).active == 1);
  CHECK(rule_style(rules, st("20", "Quantity", 20, true), 0).active == 0);
  CHECK(rule_style(rules, st("50", "Quantity", 50, true), 0).active == -1);
  cJSON_Delete(rules);
  rules = cJSON_Parse(R"([{"equals":"0","layout":"center"},{"layout":"corner","active":true}])");
  CHECK(rule_style(rules, st("0", "Percent", 0, true), 0).layout == "center");
  CHECK(rule_style(rules, st("36", "Percent", 36, true), 0).layout == "corner");
  cJSON_Delete(rules);
  // at most / at least, the thresholds from a member's metadata ("@key"; as text, as the firmware keeps it)
  rules = cJSON_Parse(R"([{"at_most":"@critical","color":"red"},{"at_most":"@warn","color":"yellow"},
                          {"at_least":40,"color":"blue"},{"color":"green"}])");
  {
    cJSON *meta = cJSON_Parse(R"({"warn":"14","critical":"8"})");
    CHECK(rule_style(rules, st("8", "Decimal", 8, true), 0, meta).color == 63488);     // <= 8
    CHECK(rule_style(rules, st("14", "Decimal", 14, true), 0, meta).color == 65472);   // <= 14
    CHECK(rule_style(rules, st("15", "Decimal", 15, true), 0, meta).color == 21920);
    CHECK(rule_style(rules, st("40", "Decimal", 40, true), 0, meta).color == 1055);    // >= 40
    CHECK(rule_style(rules, st("3", "Decimal", 3, true), 0).color == 21920);           // no metadata: no threshold
    CHECK(rule_style(rules, st("UNDEF", "UnDef"), 0, meta).color == 21920);            // no number: the fallback
    cJSON_Delete(meta);
  }
  cJSON_Delete(rules);
  rules = cJSON_Parse(R"([{"within_days":1,"color":"yellow"}])");
  const time_t now = 1791196439;  // Mon 5 Oct 2026, 12:33 local
  CHECK(rule_style(rules, st("x", "Decimal", now + 86400, true), now).color == 65472);      // tomorrow
  CHECK(rule_style(rules, st("x", "Decimal", now + 3 * 86400, true), now).color == -1);     // in three days
  CHECK(rule_style(rules, st("x", "Decimal", now - 86400, true), now).color == -1);         // yesterday
  cJSON_Delete(rules);
  // times of day: the evening before from 17:00, the day itself until 08:00 (a bin to put out)
  rules = cJSON_Parse(R"([{"within_days":1,"from":"17:00","until":"08:00","color":"yellow"}])");
  CHECK(rule_style(rules, st("x", "Decimal", now + 86400, true), now).color == -1);                   // tomorrow, 12:33
  CHECK(rule_style(rules, st("x", "Decimal", now + 86400, true), now + 5 * 3600).color == 65472);     // tomorrow, 17:33
  CHECK(rule_style(rules, st("x", "Decimal", now, true), now - 5 * 3600).color == 65472);             // today, 07:33
  CHECK(rule_style(rules, st("x", "Decimal", now, true), now).color == -1);                           // today, 12:33
  CHECK(clock_minutes("08:00") == 480 && clock_minutes("8") == -1 && clock_minutes(nullptr) == -1);
  cJSON_Delete(rules);
  rules = cJSON_Parse(R"([{"equals":"OPEN","color":"red"}])");
  CHECK(rule_style(rules, st("OPEN", "OpenClosed"), 0).color == 63488);
  CHECK(rule_style(rules, st("CLOSED", "OpenClosed"), 0).color == -1);
  cJSON_Delete(rules);
  rules = cJSON_Parse(R"([{"equals":"OPEN","icon":"window-open-variant","blink":500},{"above":30,"color":"red"}])");
  {
    const RuleStyle open = rule_style(rules, st("OPEN", "OpenClosed"), 0);
    CHECK(open.color == -1 && open.blink == 500 && open.icon == "window-open-variant");
    const RuleStyle none = rule_style(rules, st("CLOSED", "OpenClosed"), 0);
    CHECK(none.color == -1 && none.blink == 0 && none.icon.empty());
  }
  cJSON_Delete(rules);
  CHECK(is_on(st("ON", "OnOff")) && !is_on(st("OFF", "OnOff")) && is_on(st("40", "Percent", 40, true)));

  // the night: from..to, also across midnight
  CHECK(in_night(23 * 60 + 30, 23 * 60, 6 * 60) && in_night(5 * 60, 23 * 60, 6 * 60));
  CHECK(!in_night(6 * 60, 23 * 60, 6 * 60) && !in_night(12 * 60, 23 * 60, 6 * 60));
  CHECK(in_night(13 * 60, 12 * 60, 14 * 60) && !in_night(14 * 60, 12 * 60, 14 * 60));
  CHECK(!in_night(0, -1, -1) && !in_night(600, 600, 600));
  {
    cJSON *s = cJSON_Parse(R"({"brightness": {"value": 80, "item": "B"}, "idle_after": 0,
                               "night": {"from": "22:30", "to": "06:00"}})");
    const ScreenSettings sc = screen_settings(s);
    CHECK(sc.brightness == 80 && sc.brightness_item == "B" && sc.idle_brightness == 60 && sc.idle_after == 0);
    CHECK(sc.night_from == 22 * 60 + 30 && sc.night_to == 360 && sc.night_item.empty());
    cJSON_Delete(s);
    CHECK(screen_settings(nullptr).brightness == 85 && screen_settings(nullptr).night_from == -1);
  }

  // icons
  if (argc > 1) {
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    static std::string raw = ss.str();
    CHECK(icons_use(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));
    CHECK(icon_count() == 7651);  // MDI 7.4.47, kept old names, OHpanel's own icons
    CHECK(icon_glyph("ohpanel-equalizer-1") == std::string("\xEF\xBC\xAA"));  // U+FF2A (the icons from "battery" to "dryer" sort first)
    CHECK(icon_glyph("desk-lamp") == std::string("\xEE\xA5\x9E"));
    CHECK(icon_glyph("close-circle-outline") == std::string("\xEE\x85\x99"));
    CHECK(icon_glyph("lecturn") == icon_glyph("lectern"));  // an old name, an alias now
    CHECK(icon_glyph("no-such-icon").empty());
  }

  // the panel's NTC
  CHECK(std::fabs(ntc::celsius(3300.0 * 10000 / 21200) - 25.0) < 0.01);  // 10 kOhm
  CHECK(std::fabs(ntc::celsius(3300.0 * 5000 / 16200) - 41.5) < 0.1);    // 5 kOhm
  CHECK(std::isnan(ntc::celsius(0)) && std::isnan(ntc::celsius(3300)));

  // QR code for the hotspot
  CHECK(wifi_qr_text("ohpanel-a1b2c3", "aB3$xY7%") == "WIFI:T:WPA;S:ohpanel-a1b2c3;P:aB3$xY7%;;");
  CHECK(wifi_qr_text("a;b:c", "p,w") == "WIFI:T:WPA;S:a\\;b\\:c;P:p\\,w;;");
  CHECK(wifi_qr_text("ohpanel-a1b2c3", "") == "WIFI:S:ohpanel-a1b2c3;;");   // an open network: no type

  // notifications: level|title|text and the shorter forms
  {
    Notification n = parse_notification("warning|Window open|The bathroom window is open");
    CHECK(n.level == "warning" && n.title == "Window open" && n.text == "The bathroom window is open");
    n = parse_notification("ALERT|Water!");
    CHECK(n.level == "alert" && n.title.empty() && n.text == "Water!");
    n = parse_notification("Post|A letter came");
    CHECK(n.level == "info" && n.title == "Post" && n.text == "A letter came");
    n = parse_notification("Post|a | b");
    CHECK(n.level == "info" && n.title == "Post" && n.text == "a | b");
    n = parse_notification("just text\\nsecond line");
    CHECK(n.level == "info" && n.title.empty() && n.text == "just text\nsecond line");
    n = parse_notification("loud|T|x");
    CHECK(n.level == "info" && n.title == "loud" && n.text == "T|x");
  }

  if (g_failures) {
    std::fprintf(stderr, "%d format check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("format: all checks passed\n");
  return 0;
}
