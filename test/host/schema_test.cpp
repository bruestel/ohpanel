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
  // a hardware button that opens a page: nothing else; its relay left alone or on an item of its own
  const auto relay_page = [](const std::string &relay) {
    return doc(R"("pages":[{"id":"bus","type":"entities","title":"x","rows":[{"item":"A"}]}],"home":{"relays":[)" + relay + "]}");
  };
  expect_valid("button opens a page", relay_page(R"({"label":"Bus","open":"bus"})"));
  expect_valid("button opens a page, relay on its item", relay_page(R"({"label":"Bus","open":"bus","relay":{"item":"R"}})"));
  expect_valid("button opens the settings", relay_page(R"({"label":"Set","open":"settings","relay":false})"));
  expect_issue("button opens no page", relay_page(R"({"label":"Bus","open":"nowhere"})"), "home.relays[0].open");
  // the weather from one item a rule writes as JSON: the page's days, home's weather block
  expect_valid("weather feed", doc(R"("pages":[{"id":"w","type":"weather","title":"x","feed":{"item":"Wx","codes":"bom"}}])"));
  expect_issue("weather feed and days", doc(R"("pages":[{"id":"w","type":"weather","title":"x","feed":{"item":"Wx"},"days":[]}])"),
               "pages[0].days");
  expect_issue("weather feed no item", doc(R"("pages":[{"id":"w","type":"weather","title":"x","feed":{"codes":"bom"}}])"),
               "pages[0].feed.item");
  expect_valid("home weather feed", doc(R"("home":{"weather_feed":{"item":"Wx","codes":"wmo"}})"));
  expect_issue("home weather feed codes", doc(R"("home":{"weather_feed":{"item":"Wx","codes":"B O M"}})"),
               "home.weather_feed.codes");
  // a tap on the clock: a page
  const auto clock_page = [](const std::string &to) {
    return doc(R"("pages":[{"id":"bus","type":"entities","title":"x","rows":[{"item":"A"}]}],"home":{"clock_page":)" + to + "}");
  };
  expect_valid("clock opens a page", clock_page(R"("bus")"));
  expect_valid("clock opens the settings", clock_page(R"("settings")"));
  expect_issue("clock opens no page", clock_page(R"("nowhere")"), "home.clock_page");
  expect_issue("clock opens an item's page", clock_page(R"({"item":"Clock_Target"})"), "home.clock_page");
  expect_issue("clock opens a number", clock_page("3"), "home.clock_page");
  expect_issue("button opens a page and an item", relay_page(R"({"label":"Bus","open":"bus","item":"A"})"),
               "home.relays[0].item");
  expect_issue("button opens a page and switches", relay_page(R"({"label":"Bus","open":"bus","relay":true})"),
               "home.relays[0].relay");
  // an entity page's feed: one String item holds the list as JSON
  const auto feed_page = [](const std::string &rest) {
    return doc(R"("pages":[{"id":"p","type":"entities","title":"x",)" + rest + "}]");
  };
  expect_valid("feed", feed_page(R"("feed":{"item":"Bus"})"));
  // a row's line badge: one to three letters and digits
  expect_valid("badge", feed_page(R"("rows":[{"item":"A","badge":"12"},{"item":"B","badge":"S1"},{"item":"C","badge":"re5"}])"));
  expect_issue("badge too long", feed_page(R"("rows":[{"item":"A","badge":"1234"}])"), "pages[0].rows[0].badge");
  expect_issue("badge empty", feed_page(R"("rows":[{"item":"A","badge":""}])"), "pages[0].rows[0].badge");
  expect_issue("badge with a space", feed_page(R"("rows":[{"item":"A","badge":"S 1"}])"), "pages[0].rows[0].badge");
  expect_issue("badge with an umlaut", feed_page(R"("rows":[{"item":"A","badge":"Ü1"}])"), "pages[0].rows[0].badge");
  expect_issue("badge not a text", feed_page(R"("rows":[{"item":"A","badge":12}])"), "pages[0].rows[0].badge");
  expect_valid("feed asked every minute", feed_page(R"("feed":{"item":"Bus","refresh":60,"timeout":10},"icon":"bus",)"
                                                     R"("color":"yellow","style":"rows")"));
  expect_valid("feed only pushed", feed_page(R"("feed":{"item":"Bus","ask":false})"));
  expect_valid("feed refresh 0", feed_page(R"("feed":{"item":"Bus","refresh":0})"));
  expect_valid("feed refresh 0 not asking", feed_page(R"("feed":{"item":"Bus","ask":false,"refresh":0})"));
  expect_issue("feed without item", feed_page(R"("feed":{"refresh":60})"), "pages[0].feed.item");
  expect_issue("feed with a bad item", feed_page(R"("feed":{"item":"Bus Stop"})"), "pages[0].feed.item");
  expect_issue("feed not an object", feed_page(R"("feed":"Bus")"), "pages[0].feed");
  expect_issue("feed with an unknown key", feed_page(R"("feed":{"item":"Bus","every":60})"), "pages[0].feed.every");
  expect_issue("feed refresh too often", feed_page(R"("feed":{"item":"Bus","refresh":5})"), "pages[0].feed.refresh");
  expect_issue("feed refresh not whole", feed_page(R"("feed":{"item":"Bus","refresh":60.5})"), "pages[0].feed.refresh");
  expect_issue("feed timeout too long", feed_page(R"("feed":{"item":"Bus","timeout":600})"), "pages[0].feed.timeout");
  expect_issue("feed refresh without asking", feed_page(R"("feed":{"item":"Bus","ask":false,"refresh":60})"),
               "pages[0].feed.refresh");
  expect_issue("feed timeout without asking", feed_page(R"("feed":{"item":"Bus","ask":false,"timeout":5})"),
               "pages[0].feed.timeout");
  expect_issue("feed and rows", feed_page(R"("feed":{"item":"Bus"},"rows":[{"item":"A"}])"), "pages[0]");
  expect_issue("feed and group", feed_page(R"("feed":{"item":"Bus"},"group":"gBus")"), "pages[0]");
  expect_issue("feed with rules", feed_page(R"("feed":{"item":"Bus"},"value_style":[{"equals":"x","color":"red"}])"),
               "pages[0].value_style");
  expect_issue("feed with metadata", feed_page(R"("feed":{"item":"Bus"},"metadata":"bus")"), "pages[0]");
  expect_issue("entities without rows", feed_page(R"("style":"rows")"), "pages[0]");
  expect_valid("group with metadata",
               doc("\"pages\":[{\"id\":\"p\",\"type\":\"entities\",\"title\":\"x\",\"group\":\"gPlants\","
                   "\"metadata\":\"plant\",\"label\":\"{short_name} > {value}\",\"style\":\"rows\","
                   "\"value_style\":[{\"at_most\":\"@critical\",\"color\":\"red\"},{\"at_least\":5,\"color\":\"green\"}]}]"));
  expect_valid("value rows and coloured info fields",
               doc("\"home\":{\"values\":[{\"item\":\"T\",\"icon\":\"thermometer\"},{\"item\":\"H\",\"format\":\"time\"}],"
                   "\"info\":[{\"left\":{\"icon\":\"tram\",\"color\":\"white\",\"bg\":\"red\"},"
                   "\"main\":{\"item\":\"D\",\"color\":\"blue\"},\"right\":{\"text\":\"x\",\"color\":\"yellow\"}}]}"));
  expect_valid("a relay's own item", doc("\"home\":{\"relays\":[{\"relay\":{\"item\":\"R\"}},"
                                         "{\"item\":\"B\",\"relay\":{\"item\":\"R2\"},\"hidden\":true}]}"));
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
  expect_valid("thermostat unit", doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","unit":"%"}])"));
  expect_valid("media clock", doc(R"("pages":[{"id":"m","type":"media","title":"x","control":{"item":"P"},"clock":true}])"));
  expect_issue("media clock flag",
               doc(R"("pages":[{"id":"m","type":"media","title":"x","control":{"item":"P"},"clock":"yes"}])"), "pages[0].clock");
  expect_issue("thermostat unit too long",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","unit":"percent"}])"), "pages[0].unit");
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
  expect_valid("keypad toggle button",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","buttons":[{"label":"Arm","command":"ARM {code}"},
                   {"label":"Light","icon":"lightbulb","action":"toggle","item":"Light","on":"ON"}]}])"));
  expect_issue("keypad toggle without its item",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","buttons":[{"label":"L","action":"toggle"}]}])"),
               "pages[0].buttons[0].item");
  expect_issue("keypad toggle with a command",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","buttons":[{"label":"L","action":"toggle","item":"L",
                   "command":"ON"}]}])"),
               "pages[0].buttons[0].command");
  expect_issue("keypad on without toggle",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","buttons":[{"label":"L","on":"ON"}]}])"),
               "pages[0].buttons[0].on");
  expect_issue("keypad action unknown",
               doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","buttons":[{"label":"L","action":"open"}]}])"),
               "pages[0].buttons[0].action");
  expect_issue("keypad length", doc(R"("pages":[{"id":"a","type":"keypad","title":"x","item":"C","length":9,
                   "buttons":[{"label":"Open"}]}])"),
               "pages[0].length");
  expect_valid("thermostat with on / off",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","switch":{"item":"P","locked":true}}])"));
  expect_valid("thermostat switch with its label and icon",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","switch":{"item":"B","label":"Boost","icon":"fire"}}])"));
  expect_issue("thermostat switch label too long",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","switch":{"item":"B","label":"a label far too long"}}])"),
               "pages[0].switch.label");
  expect_issue("thermostat switch locked not a boolean",
               doc(R"("pages":[{"id":"t","type":"thermostat","title":"x","item":"S","switch":{"item":"P","locked":1}}])"),
               "pages[0].switch.locked");
  expect_valid("image page from an item", doc(R"("pages":[{"id":"c","type":"image","title":"Cam","item":"CamPic","pixel":2}],"swipe":["home","c"])"));
  expect_valid("image page from an address",
               doc(R"("pages":[{"id":"c","type":"image","title":"Cam","url":"http://cam/latest.jpg?h=320","refresh":5}])"));
  expect_issue("image page without a picture", doc(R"("pages":[{"id":"c","type":"image","title":"Cam"}])"), "pages[0].item");
  expect_issue("image page with item and url",
               doc(R"("pages":[{"id":"c","type":"image","title":"Cam","item":"A","url":"/static/a.jpg"}])"), "pages[0].url");
  expect_issue("image page url not an address",
               doc(R"("pages":[{"id":"c","type":"image","title":"Cam","url":"cam/latest.jpg"}])"), "pages[0].url");
  expect_issue("image page pixel out of range",
               doc(R"("pages":[{"id":"c","type":"image","title":"Cam","item":"A","pixel":1}])"), "pages[0].pixel");
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
  expect_valid("keypad stay", doc("\"pages\":[{\"id\":\"k\",\"type\":\"keypad\",\"title\":\"K\",\"item\":\"I\",\"stay\":true,"
                                   "\"buttons\":[{\"label\":\"Off\"}]}]"));
  expect_issue("keypad stay not bool", doc("\"pages\":[{\"id\":\"k\",\"type\":\"keypad\",\"title\":\"K\",\"item\":\"I\","
                                           "\"stay\":1,\"buttons\":[{\"label\":\"Off\"}]}]"), "pages[0].stay");
  expect_valid("open page", doc("\"display\":{\"open_page\":{\"item\":\"Panel_Page\"}}"));
  expect_issue("open page without item", doc("\"display\":{\"open_page\":{}}"), "display.open_page.item");
  expect_issue("open page bad item", doc("\"display\":{\"open_page\":{\"item\":\"a b\"}}"), "display.open_page.item");
  expect_valid("current page", doc("\"display\":{\"current_page\":{\"item\":\"Panel_Shown\"}}"));
  expect_issue("current page without item", doc("\"display\":{\"current_page\":{}}"), "display.current_page.item");
  expect_issue("current page unknown member", doc("\"display\":{\"current_page\":{\"item\":\"X\",\"y\":1}}"),
               "display.current_page.y");
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
  expect_valid("a group's button page",
               doc("\"pages\":[{\"id\":\"l\",\"type\":\"buttons\",\"title\":\"x\",\"group\":\"gLights\",\"icon\":\"lightbulb\","
                   "\"only_on\":true,\"metadata\":\"light\",\"label\":\"{room}\"}]"));
  expect_issue("buttons beside a group",
               doc("\"pages\":[{\"id\":\"l\",\"type\":\"buttons\",\"title\":\"x\",\"group\":\"g\",\"buttons\":[]}]"), "pages[0]");
  expect_issue("only_on without a group",
               doc("\"pages\":[{\"id\":\"l\",\"type\":\"buttons\",\"title\":\"x\",\"only_on\":true}]"), "pages[0].only_on");
  expect_issue("only_on not a boolean",
               doc("\"pages\":[{\"id\":\"l\",\"type\":\"buttons\",\"title\":\"x\",\"group\":\"g\",\"only_on\":1}]"), "pages[0].only_on");
  expect_valid("settings page opened", doc("\"home\":{\"shortcuts\":[{\"icon\":\"cog\",\"open\":\"settings\"}]}"));
  expect_valid("the older open_when_off still accepted",
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
  expect_valid("sound", doc("\"display\":{\"sound\":{\"touch_volume\":{\"value\":30,\"item\":\"Panel_Volume\"},"
                            "\"notification_volume\":90,"
                            "\"touch\":\"click\",\"notifications\":{\"info\":\"rising\",\"warning\":\"none\","
                            "\"alert\":\"alarm\"},\"play\":{\"item\":\"Panel_Sound\"}}}"));
  expect_valid("sound: silent", doc("\"display\":{\"sound\":{\"touch_volume\":0}}"));
  expect_issue("sound: too loud", doc("\"display\":{\"sound\":{\"notification_volume\":101}}"),
               "display.sound.notification_volume");
  expect_issue("sound: one volume", doc("\"display\":{\"sound\":{\"volume\":50}}"), "display.sound.volume");
  expect_issue("sound: no such tone", doc("\"display\":{\"sound\":{\"touch\":\"bang\"}}"), "display.sound.touch");
  expect_issue("sound: no such level", doc("\"display\":{\"sound\":{\"notifications\":{\"error\":\"beep\"}}}"),
               "display.sound.notifications.error");
  expect_issue("sound: play item", doc("\"display\":{\"sound\":{\"play\":{\"item\":\"a b\"}}}"),
               "display.sound.play.item");
  // melodies of one's own: a tone may name one, each must be a melody under a name no tone has; at most 8
  expect_valid("sound: own melodies", doc("\"display\":{\"sound\":{\"melodies\":{\"doorbell\":\"d:d=8,o=6,b=180:e,c,4g\","
                                           "\"siren_2\":\"::c7,a6\"},\"touch\":\"doorbell\","
                                           "\"notifications\":{\"alert\":\"siren_2\"}}}"));
  expect_issue("sound: no such melody", doc("\"display\":{\"sound\":{\"melodies\":{\"doorbell\":\"::c\"},"
                                            "\"notifications\":{\"info\":\"chime\"}}}"),
               "display.sound.notifications.info");
  expect_issue("sound: no melody", doc("\"display\":{\"sound\":{\"melodies\":{\"doorbell\":\"::c,x\"}}}"),
               "display.sound.melodies.doorbell");
  expect_issue("sound: a melody's number", doc("\"display\":{\"sound\":{\"melodies\":{\"doorbell\":5}}}"),
               "display.sound.melodies.doorbell");
  expect_issue("sound: a tone's name", doc("\"display\":{\"sound\":{\"melodies\":{\"beep\":\"::c\"}}}"),
               "display.sound.melodies.beep");
  expect_issue("sound: a melody's name", doc("\"display\":{\"sound\":{\"melodies\":{\"Door Bell\":\"::c\"}}}"),
               "display.sound.melodies.Door Bell");
  expect_issue("sound: melodies a list", doc("\"display\":{\"sound\":{\"melodies\":[\"::c\"]}}"),
               "display.sound.melodies");
  {
    std::string many;
    for (int k = 0; k < 9; k++)
      many += std::string(k ? "," : "") + "\"m" + std::to_string(k) + "\":\"::c\"";
    expect_issue("sound: too many melodies", doc("\"display\":{\"sound\":{\"melodies\":{" + many + "}}}"),
                 "display.sound.melodies");
  }
  expect_valid("home after: never", doc("\"display\":{\"home_after\":0}"));
  expect_issue("home after: negative", doc("\"display\":{\"home_after\":-1}"), "display.home_after");
  expect_issue("home after: too long", doc("\"display\":{\"home_after\":3601}"), "display.home_after");
  expect_issue("home after: fraction", doc("\"display\":{\"home_after\":1.5}"), "display.home_after");
  expect_issue("home after: text", doc("\"display\":{\"home_after\":\"60\"}"), "display.home_after");
  expect_issue("weekday count", doc("\"display\":{\"texts\":{\"weekdays\":[\"a\"]}}"), "display.texts.weekdays");
  expect_issue("too many relays", doc("\"home\":{\"relays\":[{},{},{}]}"), "home.relays");
  expect_valid("notifications that wake from a level",
               doc("\"display\":{\"notifications\":{\"item\":\"N\",\"wake_and_keep_on\":\"alert\"}}"));
  expect_issue("a level that is none", doc("\"display\":{\"notifications\":{\"item\":\"N\",\"wake_and_keep_on\":\"error\"}}"),
               "display.notifications.wake_and_keep_on");
  expect_valid("on above a limit", doc("\"home\":{\"shortcuts\":[{\"item\":\"P\",\"icon\":\"cog\",\"on\":{\"above\":7}}]}"));
  expect_issue("a limit that is no number",
               doc("\"home\":{\"shortcuts\":[{\"item\":\"P\",\"icon\":\"cog\",\"on\":{\"above\":\"7\"}}]}"),
               "home.shortcuts[0].on");
  expect_issue("a limit of no kind", doc("\"home\":{\"shortcuts\":[{\"item\":\"P\",\"icon\":\"cog\",\"on\":{\"over\":7}}]}"),
               "home.shortcuts[0].on");
  {  // twelve shortcuts, three rows of four
    std::string twelve;
    for (int i = 0; i < 12; i++)
      twelve += std::string(i ? "," : "") + "{\"icon\":\"cog\"}";
    expect_valid("twelve shortcuts", doc("\"home\":{\"shortcuts\":[" + twelve + "]}"));
    expect_issue("thirteen shortcuts", doc("\"home\":{\"shortcuts\":[" + twelve + ",{\"icon\":\"cog\"}]}"),
                 "home.shortcuts");
  }
  expect_issue("relay flag", doc("\"home\":{\"relays\":[{\"label\":\"x\",\"relay\":\"no\"}]}"), "home.relays[0].relay");
  expect_issue("relay hidden flag", doc("\"home\":{\"relays\":[{\"hidden\":\"yes\"}]}"), "home.relays[0].hidden");
  expect_issue("relay without its item", doc("\"home\":{\"relays\":[{\"relay\":{}}]}"), "home.relays[0].relay.item");
  expect_issue("relay's item", doc("\"home\":{\"relays\":[{\"relay\":{\"item\":\"a b\"}}]}"), "home.relays[0].relay.item");
  expect_issue("relay's empty item", doc("\"home\":{\"relays\":[{\"relay\":{\"item\":\"\"}}]}"), "home.relays[0].relay.item");
  expect_issue("relay's unknown key", doc("\"home\":{\"relays\":[{\"relay\":{\"item\":\"A\",\"x\":1}}]}"), "home.relays[0].relay.x");
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
