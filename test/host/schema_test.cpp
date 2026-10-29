/*
   Host test for the panel configuration validator: the office example must
   pass, and broken documents must be refused with the right path.
*/

#include "panel/Schema.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

std::string read(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void expect_valid(const std::string &name, const std::string &json) {
  std::vector<panel::Issue> issues;
  if (!panel::validate(json, issues)) {
    std::fprintf(stderr, "FAIL %s: expected valid, got:\n", name.c_str());
    for (const auto &i : issues)
      std::fprintf(stderr, "    %s: %s\n", i.path.c_str(), i.message.c_str());
    g_failures++;
  }
}

/* Refused, with an issue at exactly `path`. */
void expect_issue(const std::string &name, const std::string &json, const std::string &path) {
  std::vector<panel::Issue> issues;
  const bool valid = panel::validate(json, issues);
  bool found = false;
  for (const auto &i : issues)
    found = found || i.path == path;
  if (valid || !found) {
    std::fprintf(stderr, "FAIL %s: expected an issue at \"%s\", got:\n", name.c_str(), path.c_str());
    for (const auto &i : issues)
      std::fprintf(stderr, "    %s: %s\n", i.path.c_str(), i.message.c_str());
    g_failures++;
  }
}

std::string doc(const std::string &body) { return "{\"version\":1," + body + "}"; }

}  // namespace

int main() {
  const std::string office = read("test/host/data/office.json");
  expect_valid("office example", office);
  expect_valid("minimal", "{\"version\":1}");
  expect_valid("group with metadata",
               doc("\"pages\":[{\"id\":\"p\",\"type\":\"entities\",\"title\":\"x\",\"group\":\"gPlants\","
                   "\"metadata\":\"plant\",\"label\":\"{short_name} > {value}\",\"style\":\"rows\","
                   "\"value_style\":[{\"at_most\":\"@critical\",\"color\":\"red\"},{\"at_least\":5,\"color\":\"green\"}]}]"));
  expect_valid("value rows and coloured info fields",
               doc("\"home\":{\"values\":[{\"item\":\"T\",\"icon\":\"thermometer\"},{\"item\":\"H\",\"format\":\"time\"}],"
                   "\"info\":[{\"left\":{\"icon\":\"tram\",\"color\":\"white\",\"bg\":\"red\"},"
                   "\"main\":{\"item\":\"D\",\"color\":\"blue\"},\"right\":{\"text\":\"x\",\"color\":\"yellow\"}}]}"));
  expect_valid("indoor_temperature, the older name", doc("\"home\":{\"indoor_temperature\":{\"item\":\"T\"}}"));
  expect_valid("states counting as on", doc("\"home\":{\"shortcuts\":[{\"item\":\"S\",\"icon\":\"speaker\","
                                            "\"on\":\"PLAYING\"}],\"status_icons\":[{\"item\":\"P\",\"icon\":\"speaker\","
                                            "\"on\":[\"PLAYING\",\"BUFFERING\"]}]}"));
  expect_valid("icons shown in turn", doc("\"home\":{\"shortcuts\":[{\"item\":\"S\",\"icon\":[\"equalizer\",\"poll\"],"
                                          "\"interval\":400}]}"));
  expect_issue("an interval too short", doc("\"home\":{\"shortcuts\":[{\"icon\":[\"equalizer\",\"poll\"],\"interval\":50}]}"),
               "home.shortcuts[0].interval");
  expect_issue("a list of one icon", doc("\"home\":{\"status_icons\":[{\"icon\":[\"equalizer\"]}]}"),
               "home.status_icons[0].icon");
  expect_issue("an empty on state", doc("\"home\":{\"shortcuts\":[{\"item\":\"S\",\"icon\":\"speaker\",\"on\":\"\"}]}"),
               "home.shortcuts[0].on");
  expect_valid("today's high and low", doc("\"home\":{\"outdoor_temperature\":{\"item\":\"T\"},"
                                           "\"outdoor_high\":{\"item\":\"Tmax\",\"decimals\":1},"
                                           "\"outdoor_low\":{\"item\":\"Tmin\"}}"));
  expect_issue("a value row needs an item", doc("\"home\":{\"values\":[{\"icon\":\"thermometer\"}]}"),
               "home.values[0].item");
  expect_issue("three value rows", doc("\"home\":{\"values\":[{\"item\":\"a\"},{\"item\":\"b\"},{\"item\":\"c\"}]}"),
               "home.values");
  expect_issue("an icon only in the left field", doc("\"home\":{\"info\":[{\"main\":{\"icon\":\"tram\"}}]}"),
               "home.info[0].main.icon");
  expect_issue("a badge only in the left field", doc("\"home\":{\"info\":[{\"main\":{\"text\":\"x\",\"bg\":\"red\"}}]}"),
               "home.info[0].main.bg");
  expect_issue("a background colour from the palette", doc("\"home\":{\"info\":[{\"left\":{\"text\":\"1\",\"bg\":\"pink\"}}]}"),
               "home.info[0].left.bg");

  expect_issue("not json", "{", "");
  expect_issue("wrong version", "{\"version\":2}", "version");
  expect_issue("unknown top-level member", doc("\"pagse\":[]"), "pagse");
  expect_issue("duplicate page id",
               doc("\"pages\":[{\"id\":\"a\",\"type\":\"qr\",\"title\":\"x\",\"text\":\"t\"},"
                   "{\"id\":\"a\",\"type\":\"qr\",\"title\":\"y\",\"text\":\"t\"}]"),
               "pages[1].id");
  expect_issue("bad page id", doc("\"pages\":[{\"id\":\"Bad Id\",\"type\":\"qr\",\"title\":\"x\",\"text\":\"t\"}]"),
               "pages[0].id");
  expect_issue("unknown type", doc("\"pages\":[{\"id\":\"a\",\"type\":\"clock\",\"title\":\"x\"}]"), "pages[0].type");
  expect_issue("open to a missing page",
               doc("\"pages\":[{\"id\":\"b\",\"type\":\"buttons\",\"title\":\"x\",\"buttons\":["
                   "null,{\"icon\":\"lamp\",\"open\":\"nowhere\"}]}]"),
               "pages[0].buttons[1].open");
  expect_issue("too many buttons",
               doc("\"pages\":[{\"id\":\"b\",\"type\":\"buttons\",\"title\":\"x\",\"buttons\":"
                   "[null,null,null,null,null,null,null,null,null]}]"),
               "pages[0].buttons");
  expect_issue("toggle without item",
               doc("\"pages\":[{\"id\":\"b\",\"type\":\"buttons\",\"title\":\"x\",\"buttons\":["
                   "{\"icon\":\"lamp\",\"action\":\"toggle\"}]}]"),
               "pages[0].buttons[0].item");
  expect_issue("bad item name",
               doc("\"pages\":[{\"id\":\"c\",\"type\":\"cover\",\"title\":\"x\",\"item\":\"has space\"}]"),
               "pages[0].item");
  expect_valid("cover with a command item",
               doc(R"("pages":[{"id":"c","type":"cover","item":"Pos","commands":{"item":"Cmd","open":"open"}}])"));
  expect_issue("cover command item not an item name",
               doc(R"("pages":[{"id":"c","type":"cover","item":"Pos","commands":{"item":"no way"}}])"),
               "pages[0].commands.item");
  expect_valid("thermostat",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"Set","current":{"item":"Now"},
                   "heating":{"item":"H"},"cooling":{"item":"C"},"mode":{"item":"M"},"style":"gradient",
                   "min":10,"max":28,"step":0.5,"curve":0.75,"modes":[{"label":"Auto","value":"auto"},{"value":"off"}]}])"));
  expect_valid("thermostat lock",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S",
                   "mode":{"item":"M","value_style":[{"equals":"schedule","locked":true}]}}])"));
  expect_issue("thermostat lock rule with a colour",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S",
                   "mode":{"item":"M","value_style":[{"equals":"away","color":"red"}]}}])"),
               "pages[0].mode.value_style[0].color");
  expect_issue("thermostat curve", doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","curve":0.6}])"),
               "pages[0].curve");
  expect_issue("thermostat without target", doc(R"("pages":[{"id":"t","type":"thermostat","title":"x"}])"),
               "pages[0].item");
  expect_valid("keypad",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"Alarm","item":"Cmd","length":6,"keys":"phone",
                   "state":{"item":"St","value_style":[{"equals":"ALARM","format":"Alarm!","icon":"shield-alert",
                   "color":"red","blink":500}]},"result":{"item":"Msg"},
                   "buttons":[{"label":"Away","icon":"shield-lock","command":"ARM_AWAY {code}","when":"ARMED_AWAY"},
                              {"label":"Home","item":"Home","command":"ON","code":false},
                              {"label":"Off","command":"DISARM {code}","show_when":["ARMED_AWAY","ARMED_HOME"]}]}])"));
  expect_issue("keypad show_when without a state",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C",
                   "buttons":[{"label":"Off","show_when":"ARMED"}]}])"),
               "pages[0].buttons[0].show_when");
  expect_valid("keypad, a door code",
               doc(R"("pages":[{"id":"d","type":"keypad","title":"Door","text":"Door code","icon":"lock",
                   "buttons":[{"label":"Open","item":"Door_Code"}]}])"));
  expect_issue("keypad without buttons", doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C"}])"),
               "pages[0].buttons");
  expect_issue("keypad with five buttons",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","buttons":[{"label":"1"},
                   {"label":"2"},{"label":"3"},{"label":"4"},{"label":"5"}]}])"),
               "pages[0].buttons");
  expect_issue("keypad button without an item",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","buttons":[{"label":"Open"}]}])"),
               "pages[0].buttons[0].item");
  expect_issue("keypad length", doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","length":9,
                   "buttons":[{"label":"Open"}]}])"),
               "pages[0].length");
  expect_issue("thermostat modes without mode",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","modes":[{"value":"a"}]}])"),
               "pages[0].modes");
  expect_issue("thermostat max under min",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","min":20,"max":10}])"),
               "pages[0].max");
  expect_issue("typo in member",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"rows\":[{\"itme\":\"A\"}]}]"),
               "pages[0].rows[0].itme");
  expect_issue("rows and group",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"rows\":[],\"group\":\"g\"}]"),
               "pages[0]");
  expect_issue("threshold that is no number",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"rows\":[{\"item\":\"A\","
                   "\"value_style\":[{\"at_most\":\"warn\",\"color\":\"red\"}]}]}]"),
               "pages[0].rows[0].value_style[0].at_most");
  expect_issue("metadata without a group",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"rows\":[],\"metadata\":\"plant\"}]"),
               "pages[0]");
  expect_issue("unknown entity style",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"style\":\"grid\",\"rows\":[]}]"),
               "pages[0].style");
  expect_issue("fallback rule not last",
               doc("\"pages\":[{\"id\":\"e\",\"type\":\"entities\",\"title\":\"x\",\"rows\":[{\"item\":\"A\","
                   "\"value_style\":[{\"color\":\"red\"},{\"below\":1,\"color\":\"blue\"}]}]}]"),
               "pages[0].rows[0].value_style[0]");
  expect_issue("bad color", doc("\"home\":{\"status_icons\":[{\"icon\":\"x\",\"color\":\"pink\"}]}"),
               "home.status_icons[0].color");
  expect_valid("theme color", doc("\"home\":{\"status_icons\":[{\"icon\":\"x\",\"color\":\"theme-accent\"}]}"));
  expect_issue("theme color in the theme", doc("\"display\":{\"theme\":{\"accent\":\"theme-text\"}}"),
               "display.theme.accent");
  expect_issue("swipe to a missing page", doc("\"swipe\":[\"home\",\"gone\"]"), "swipe[1]");
  expect_issue("weather day", doc("\"pages\":[{\"id\":\"w\",\"type\":\"weather\",\"title\":\"x\",\"day\":9,"
                                  "\"icon\":{\"item\":\"I\"}}]"),
               "pages[0].day");
  expect_issue("qr too long",
               doc("\"pages\":[{\"id\":\"q\",\"type\":\"qr\",\"title\":\"x\",\"text\":\"" + std::string(201, 'a') + "\"}]"),
               "pages[0].text");
  expect_issue("qr style",
               doc("\"pages\":[{\"id\":\"q\",\"type\":\"qr\",\"title\":\"x\",\"text\":\"t\",\"style\":\"round\"}]"),
               "pages[0].style");

  expect_valid("display", doc("\"display\":{\"language\":\"de\",\"date_format\":\"%a, %e. %b %y\","
                                "\"texts\":{\"on\":\"Ein\",\"weekdays_short\":[\"So\",\"Mo\",\"Di\",\"Mi\",\"Do\",\"Fr\",\"Sa\"]}}"));
  expect_issue("unknown language", doc("\"display\":{\"language\":\"xx\"}"), "display.language");
  expect_valid("display: French", doc("\"display\":{\"language\":\"fr\"}"));
  expect_valid("display: Polish", doc("\"display\":{\"language\":\"pl\"}"));
  expect_valid("home after", doc("\"display\":{\"home_after\":120}"));
  expect_valid("page fade", doc("\"display\":{\"page_fade\":100}"));
  expect_issue("page fade not ms", doc("\"display\":{\"page_fade\":true}"), "display.page_fade");
  expect_valid("format pattern", doc("\"home\":{\"values\":[{\"item\":\"P\",\"format\":\"%,.2f kW (%%)\",\"factor\":0.001},"
                                     "{\"item\":\"T\",\"format\":\"%d %unit%\"}]}"));
  expect_issue("format unknown placeholder", doc("\"home\":{\"values\":[{\"item\":\"P\",\"format\":\"%x W\"}]}"),
               "home.values[0].format");
  expect_valid("format a fixed text", doc("\"home\":{\"values\":[{\"item\":\"P\",\"value_style\":[{\"equals\":\"1\","
                                         "\"format\":\"On\"}]}]}"));
  expect_valid("value row rules", doc("\"home\":{\"values\":[{\"item\":\"P\",\"value_style\":[{\"above\":1000,"
                                       "\"format\":\"%.2f kW\",\"factor\":0.001,\"color\":\"red\"}]}]}"));
  expect_valid("rule icon colour", doc("\"home\":{\"values\":[{\"item\":\"P\",\"value_style\":[{\"above\":1,"
                                        "\"icon_color\":\"#ff8800\"}]}]}"));
  expect_issue("rule above a text", doc("\"home\":{\"values\":[{\"item\":\"P\",\"value_style\":[{\"above\":\"test\","
                                        "\"color\":\"red\"}]}]}"),
               "home.values[0].value_style[0].above");
  expect_issue("rule factor alone", doc("\"home\":{\"values\":[{\"item\":\"P\",\"value_style\":[{\"above\":1,"
                                        "\"factor\":2,\"color\":\"red\"}]}]}"),
               "home.values[0].value_style[0].factor");
  expect_issue("factor zero", doc("\"home\":{\"values\":[{\"item\":\"P\",\"factor\":0}]}"), "home.values[0].factor");
  // the settings page is built in, but has no page dots: not in the swipe row; nor a media, cover or light page
  expect_issue("settings page not in the swipe row", doc("\"swipe\":[\"settings\"]"), "swipe[0]");
  expect_issue("light page not in the swipe row",
               doc("\"pages\":[{\"id\":\"l\",\"type\":\"light\",\"title\":\"x\",\"brightness\":{\"item\":\"L\"}}],"
                   "\"swipe\":[\"home\",\"l\"]"),
               "swipe[1]");
  // the media page's cover and favourites page; the favourites need their catalogue and play item
  const std::string fav = "{\"id\":\"fav\",\"type\":\"media_favorites\",\"title\":\"Radio\",\"stations\":"
                          "{\"item\":\"Stations\"},\"player\":\"m\"}";
  expect_valid("media page with cover and favourites",
               doc("\"pages\":[{\"id\":\"m\",\"type\":\"media\",\"title\":\"x\",\"control\":{\"item\":\"C\"},"
                   "\"art\":{\"item\":\"A\"},\"favorites\":\"fav\",\"play\":{\"item\":\"P\"}}," + fav + "]"));
  expect_issue("media page's favourites missing",
               doc("\"pages\":[{\"id\":\"m\",\"type\":\"media\",\"title\":\"x\",\"control\":{\"item\":\"C\"},"
                   "\"favorites\":\"nope\",\"play\":{\"item\":\"P\"}}]"),
               "pages[0].favorites");
  expect_issue("favourites without play item",
               doc("\"pages\":[{\"id\":\"m\",\"type\":\"media\",\"title\":\"x\",\"control\":{\"item\":\"C\"},"
                   "\"favorites\":\"fav\"}," + fav + "]"),
               "pages[0].play");
  expect_issue("favourites without catalogue",
               doc("\"pages\":[{\"id\":\"fav\",\"type\":\"media_favorites\",\"title\":\"x\"}]"),
               "pages[0].stations");
  expect_issue("media page's stations gone",
               doc("\"pages\":[{\"id\":\"m\",\"type\":\"media\",\"title\":\"x\",\"control\":{\"item\":\"C\"},"
                   "\"stations\":{}}]"),
               "pages[0].stations");
  expect_valid("settings page opened", doc("\"home\":{\"shortcuts\":[{\"icon\":\"cog\",\"open\":\"settings\"}]}"));
  expect_valid("a shortcut opening while off too",
               doc("\"home\":{\"shortcuts\":[{\"item\":\"S\",\"icon\":\"cog\",\"open\":\"settings\",\"open_when_off\":true}]}"));
  expect_issue("open_when_off not a boolean",
               doc("\"home\":{\"shortcuts\":[{\"icon\":\"cog\",\"open_when_off\":1}]}"), "home.shortcuts[0].open_when_off");
  expect_issue("open_when_off on a status icon",
               doc("\"home\":{\"status_icons\":[{\"icon\":\"cog\",\"open_when_off\":true}]}"), "home.status_icons[0].open_when_off");
  // besides button pages: lists, the weather, QR codes and the info page; the first is the start page, home too only
  // when named
  expect_valid("info page in the swipe row, the start page", doc("\"swipe\":[\"info\"]"));
  expect_valid("pages of other types in the swipe row",
               doc(R"("pages":[{"id":"e","type":"entities","title":"x","rows":[{"item":"A"}]},
                   {"id":"q","type":"qr","title":"x","text":"y"}],"swipe":["e","home","q"])"));
  expect_valid("a thermostat and a keypad in the swipe row",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"T"},
                   {"id":"k","type":"keypad","title":"x","item":"K","buttons":[{"label":"Send"}]}],"swipe":["t","k"])"));
  expect_issue("a page twice in the swipe row", doc("\"swipe\":[\"home\",\"info\",\"info\"]"), "swipe[2]");
  expect_valid("info page titled", doc("\"pages\":[{\"id\":\"info\",\"type\":\"system\",\"title\":\"Panel\"}]"));
  expect_issue("info page id taken", doc("\"pages\":[{\"id\":\"info\",\"type\":\"qr\",\"title\":\"x\",\"text\":\"y\"}]"),
               "pages[0].id");
  expect_valid("value style on an indicator",
               doc("\"home\":{\"status_icons\":[{\"item\":\"W\",\"icon\":\"window-closed-variant\",\"value_style\":"
                   "[{\"equals\":\"OPEN\",\"icon\":\"window-open-variant\",\"blink\":500}]}]}"));
  expect_issue("value style rule that sets nothing",
               doc("\"home\":{\"status_icons\":[{\"item\":\"W\",\"icon\":\"alert\",\"value_style\":[{\"equals\":\"ON\"}]}]}"),
               "home.status_icons[0].value_style[0]");
  expect_issue("value style on a fixed text",
               doc("\"home\":{\"info\":[{\"main\":{\"text\":\"x\",\"value_style\":[{\"color\":\"red\"}]}}]}"),
               "home.info[0].main.value_style");
  expect_valid("blinking icon", doc("\"home\":{\"status_icons\":[{\"icon\":\"alert\",\"blink\":500}]}"));
  expect_issue("blink too fast", doc("\"home\":{\"status_icons\":[{\"icon\":\"alert\",\"blink\":100}]}"),
               "home.status_icons[0].blink");
  expect_issue("blink with several icons",
               doc("\"home\":{\"shortcuts\":[{\"icon\":[\"a\",\"b\"],\"blink\":500}]}"), "home.shortcuts[0].blink");
  expect_issue("settings page id taken", doc("\"pages\":[{\"id\":\"settings\",\"type\":\"system\"}]"), "pages[0].id");
  expect_issue("settings not a page type", doc("\"pages\":[{\"id\":\"s\",\"type\":\"settings\"}]"), "pages[0].type");
  expect_valid("screen", doc("\"display\":{\"screen\":{\"brightness\":80,\"idle_brightness\":{\"value\":10,"
                             "\"item\":\"Panel_Idle\"},\"idle_after\":{\"item\":\"Panel_After\"},"
                             "\"night\":{\"from\":\"23:00\",\"to\":\"06:30\"}}}"));
  expect_valid("screen: night by item", doc("\"display\":{\"screen\":{\"night\":{\"item\":\"Panel_Night\"}}}"));
  expect_issue("screen: brightness 0", doc("\"display\":{\"screen\":{\"brightness\":0}}"), "display.screen.brightness");
  expect_issue("screen: idle over 100", doc("\"display\":{\"screen\":{\"idle_brightness\":{\"value\":101}}}"),
               "display.screen.idle_brightness.value");
  expect_issue("screen: bad item", doc("\"display\":{\"screen\":{\"idle_after\":{\"item\":\"a b\"}}}"),
               "display.screen.idle_after.item");
  expect_valid("rule: a date with times", doc("\"pages\":[{\"id\":\"w\",\"type\":\"entities\",\"title\":\"Bins\",\"rows\":[{\"item\":\"Bin\","
                                             "\"value_style\":[{\"within_days\":1,\"from\":\"17:00\",\"until\":\"08:00\","
                                             "\"color\":\"yellow\"}]}]}]"));
  expect_issue("rule: a time without a date", doc("\"pages\":[{\"id\":\"w\",\"type\":\"entities\",\"rows\":[{\"item\":\"B\","
                                                  "\"value_style\":[{\"above\":1,\"from\":\"17:00\",\"color\":\"red\"}]}]}]"),
               "pages[0].rows[0].value_style[0].from");
  expect_issue("rule: a time", doc("\"pages\":[{\"id\":\"w\",\"type\":\"entities\",\"rows\":[{\"item\":\"B\","
                                   "\"value_style\":[{\"within_days\":1,\"until\":\"8 Uhr\",\"color\":\"red\"}]}]}]"),
               "pages[0].rows[0].value_style[0].until");
  expect_issue("screen: night time", doc("\"display\":{\"screen\":{\"night\":{\"from\":\"25:00\",\"to\":\"06:00\"}}}"),
               "display.screen.night.from");
  expect_issue("screen: night both", doc("\"display\":{\"screen\":{\"night\":{\"item\":\"N\",\"from\":\"23:00\"}}}"),
               "display.screen.night");
  expect_issue("screen: unknown", doc("\"display\":{\"screen\":{\"contrast\":1}}"), "display.screen.contrast");
  expect_valid("home after: never", doc("\"display\":{\"home_after\":0}"));
  expect_issue("home after: negative", doc("\"display\":{\"home_after\":-1}"), "display.home_after");
  expect_issue("home after: too long", doc("\"display\":{\"home_after\":3601}"), "display.home_after");
  expect_issue("home after: fraction", doc("\"display\":{\"home_after\":1.5}"), "display.home_after");
  expect_issue("home after: text", doc("\"display\":{\"home_after\":\"60\"}"), "display.home_after");
  expect_issue("weekday count", doc("\"display\":{\"texts\":{\"weekdays\":[\"a\"]}}"), "display.texts.weekdays");
  expect_issue("too many relays", doc("\"home\":{\"relays\":[{},{},{}]}"), "home.relays");
  expect_issue("relay flag", doc("\"home\":{\"relays\":[{\"label\":\"x\",\"relay\":\"no\"}]}"), "home.relays[0].relay");
  expect_issue("info field item and text",
               doc("\"home\":{\"info\":[{\"main\":{\"item\":\"A\",\"text\":\"b\"}}]}"), "home.info[0].main");
  expect_issue("icon set", doc("\"home\":{\"weather_icon\":{\"item\":\"W\",\"codes\":\"Open Weather\"}}"),
               "home.weather_icon.codes");

  if (g_failures) {
    std::fprintf(stderr, "%d schema test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("schema: all checks passed\n");
  return 0;
}
