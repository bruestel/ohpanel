/*
   Host test for the display logic: the office configuration on the built
   design, driven by the events the display sends.
*/

#include "appdisplay/Engine.h"
#include "appdisplay/Icons.h"

#include <cJSON.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <tuple>
#include <sstream>
#include <string>
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

std::string read(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Recorder : Sink {
  std::vector<std::string> out;
  void send(const std::string &c) override { out.push_back(c); }
  bool has(const std::string &c) const {
    for (const auto &x : out)
      if (x == c) return true;
    return false;
  }
  /* position of a command (its n-th occurrence, from 0), or -1 */
  int pos(const std::string &c, int n = 0) const {
    for (size_t i = 0; i < out.size(); i++)
      if (out[i] == c && n-- == 0) return static_cast<int>(i);
    return -1;
  }
  bool has_prefix(const std::string &p) const {
    for (const auto &x : out)
      if (x.rfind(p, 0) == 0) return true;
    return false;
  }
  bool has_suffix_of(const std::string &p, const std::string &suffix) const {
    for (const auto &x : out)
      if (x.rfind(p, 0) == 0 && x.size() >= suffix.size() && x.compare(x.size() - suffix.size(), suffix.size(), suffix) == 0)
        return true;
    return false;
  }
  /* the first command starting with `p`, or "" */
  std::string with_prefix(const std::string &p) const {
    for (const auto &x : out)
      if (x.rfind(p, 0) == 0) return x;
    return "";
  }
  void dump() const {
    for (const auto &x : out) std::fprintf(stderr, "    %s\n", x.c_str());
  }
};

std::map<std::string, appoh::ItemState> g_states;

void set_state(const char *item, const char *state, const char *type, const char *display = nullptr,
               double numeric = 0, bool has = false) {
  appoh::ItemState s;
  s.state = state;
  s.type = type;
  s.display = display ? display : state;
  s.numeric = numeric;
  s.has_numeric = has;
  g_states[item] = s;
}

nextion::Message event(const std::string &page, const std::string &component, const std::string &value) {
  nextion::Message m;
  m.kind = nextion::Message::Kind::Event;
  m.name = "nspanelevent";
  m.text = "{\"page\": \"" + page + "\", \"component\": \"" + component + "\", \"value\": \"" + value + "\"}";
  return m;
}
nextion::Message loaded(const std::string &page) { return event(page, "currentpage", "pagechange"); }

/* A picture id from the manifest the build wrote: background -> slot -> picture. */
int manifest_pic(const char *bg, const char *slot, const char *picture) {
  cJSON *m = cJSON_Parse(read("boards/nspanel/nextion/display-manifest-eu.json").c_str());
  const cJSON *p = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "pictures"), bg),
                                                           slot), picture);
  const int id = cJSON_IsNumber(p) ? p->valueint : -1;
  cJSON_Delete(m);
  return id;
}
int background_pic(const char *id) {
  cJSON *m = cJSON_Parse(read("boards/nspanel/nextion/display-manifest-eu.json").c_str());
  int pic = -1;
  const cJSON *bgs = cJSON_GetObjectItem(m, "backgrounds");
  for (const cJSON *b = bgs ? bgs->child : nullptr; b; b = b->next)
    if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(b, "id"))) == id)
      pic = static_cast<int>(cJSON_GetNumberValue(cJSON_GetObjectItem(b, "pic")));
  cJSON_Delete(m);
  return pic;
}
/* "page <id>" for a page of the design, its id from the manifest. */
std::string page_cmd(const char *page) {
  cJSON *m = cJSON_Parse(read("boards/nspanel/nextion/display-manifest-eu.json").c_str());
  const int id = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "pages"), page), "id")->valueint;
  cJSON_Delete(m);
  return "page " + std::to_string(id);
}
std::string pic_cmd(const char *obj, const char *bg, const char *slot, const char *picture) {
  return std::string(obj) + ".pic=" + std::to_string(manifest_pic(bg, slot, picture));
}
}  // namespace

int main(int argc, char **argv) {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  static std::string icons = read(argc > 1 ? argv[1] : "test/host/build/icons.bin");
  icons_use(reinterpret_cast<const uint8_t *>(icons.data()), icons.size());

  set_state("Office_Temperature", "23.2 °C", "Quantity", nullptr, 23.2, true);
  g_states["Office_Temperature"].unit = "°C";
  set_state("weatherCurrentConditionIconId", "10d", "String");
  set_state("weatherTodayCondition", "Light rain", "String");
  for (const auto &[item, state, value] : {std::tuple{"gOutdoorTemperature", "17.4 °C", 17.4},
                                           std::tuple{"weatherTodayMaxTemperature", "19.2 °C", 19.2},
                                           std::tuple{"weatherTodayMinTemperature", "8.6 °C", 8.6}}) {
    set_state(item, state, "Quantity", nullptr, value, true);
    g_states[item].unit = "°C";
  }
  set_state("DeskLamp_Switch", "OFF", "OnOff");
  set_state("Blind_Left", "30", "Percent", nullptr, 30, true);
  set_state("PV_Production_ActivePowerTotal", "-12 W", "Quantity", "-12 W", -12, true);
  set_state("nextDepartureLine", "92", "String");
  set_state("nextDepartureDestination", "Central Station via Market Square and Old Town", "String");  // too long for its field

  Recorder rec;
  std::vector<std::pair<std::string, std::string>> commands;
  std::vector<std::pair<int, bool>> relays;
  Callbacks cb;
  static std::map<std::string, appoh::Limits> limits;   // items' ranges in openHAB (appoh::limits)
  cb.limits = [](const std::string &item, appoh::Limits &out) {
    const auto it = limits.find(item);
    if (it != limits.end())
      out = it->second;
    return it != limits.end();
  };
  cb.state = [](const std::string &item, appoh::ItemState &out) {
    auto it = g_states.find(item);
    if (it == g_states.end()) return false;
    out = it->second;
    return true;
  };
  cb.command = [&](const std::string &item, const std::string &cmd) { commands.push_back({item, cmd}); };
  cb.relay = [&](int i, bool on) { relays.push_back({i, on}); };
  std::vector<std::pair<std::string, int>> remembered;
  cb.remember = [&](const std::string &key, int value) { remembered.push_back({key, value}); };
  static bool outdated = false;
  static std::string ssid = "home";
  // a station catalogue (appoh::options), known from the start
  static std::vector<std::pair<std::string, bool>> asked;   // options asked for: item, afresh
  cb.options = [](const std::string &item, std::vector<appoh::Option> &out, bool refresh) {
    asked.push_back({item, refresh});
    if (item != "RadioStations")
      return false;
    // Radio One with a logo (tools/station_logo.py): 2 x 2 px, a white row over a half-transparent red one
    out = {{"uri:eins", "Radio One", "", "AQICAv___wD4gAAAAgEAAAECAQE", "One", "Radio One live"},
           {"uri:info", "News Radio", "newspaper", "", "", ""}, {"uri:fritz", "Radio Three", "", "", "", ""},
           {"uri:para", "Radio Four", "palm-tree", "", "", ""}, {"uri:pd", "Radio Five", "", "", "", ""}};
    for (int k = 1; k <= 5; k++)   // more than a sheet
      out.push_back({"uri:x" + std::to_string(k), "X" + std::to_string(k), "", "", "", ""});
    return true;
  };
  // groups' members (appoh::members): unknown until set here
  static std::map<std::string, std::vector<appoh::Member>> members;
  static std::vector<std::pair<std::string, bool>> members_asked;   // group, afresh
  cb.members = [](const std::string &group, std::vector<appoh::Member> &out, bool refresh) {
    members_asked.push_back({group, refresh});
    const auto it = members.find(group);
    if (it != members.end())
      out = it->second;
    return it != members.end();
  };
  // items' own metadata (appoh::metadata): unknown until set here
  static std::map<std::string, std::map<std::string, appoh::Metadata>> metadata;
  cb.metadata = [](const std::string &item, std::map<std::string, appoh::Metadata> &out, bool) {
    const auto it = metadata.find(item);
    if (it != metadata.end())
      out = it->second;
    return it != metadata.end();
  };
  // covers asked for (Callbacks::cover): source, size, scale, radius
  static std::vector<std::tuple<std::string, int, int, int>> covers;
  cb.cover = [](const std::string &source, int size, int scale, int radius, Reveal) {
    covers.push_back({source, size, scale, radius});
  };
  cb.system = [] {
    SystemInfo i;
    i.display_outdated = outdated;
    i.version = "v1.2";
    i.uptime_s = 2 * 86400 + 3 * 3600 + 4 * 60;
    i.ip = "192.168.1.20";
    i.rssi = -61;
    i.ssid = ssid;
    i.openhab = "openhab:8080";
    i.heap_free = 145 * 1024;
    i.heap_min = 98 * 1024;
    i.psram_free = 1887437;
    i.psram_total = 2097152;
    i.temperature = 23.44;
    i.display_model = "NX4832F035_011R";
    i.display_design = "fc44925a";
    i.display_firmware = "163";
    return i;
  };

  {  // a cover as rectangles: one colour is one rectangle but at the rounded corners, which stay free
    std::vector<uint8_t> red(10 * 10 * 3);
    for (size_t i = 0; i < red.size(); i += 3)
      red[i] = 255;
    const Cover c = cover_rects(red.data(), 10, 2, 4);
    CHECK(c.side == 20 && c.rects.size() > 1 && c.rects.size() < 8);
    int area = 0;
    for (const auto &r : c.rects) {
      area += r.w * r.h;
      CHECK(r.colour == 0xF800 && r.x + r.w <= 20 && r.y + r.h <= 20);
      CHECK(!(r.y == 0 && r.x == 0));   // the top left corner left out
    }
    CHECK(area < 400 && area > 380);
    // the orders: blurred passes first (more rectangles), wiped on row by row
    std::vector<uint8_t> noise(16 * 16 * 3);
    for (size_t i = 0; i < noise.size(); i++)
      noise[i] = static_cast<uint8_t>(i * 37 % 251);
    const Cover centre = cover_rects(noise.data(), 16, 2, 4), sharpen = cover_rects(noise.data(), 16, 2, 4, Reveal::Sharpen);
    const Cover wipe = cover_rects(noise.data(), 16, 2, 4, Reveal::Wipe);
    CHECK(sharpen.rects.size() > centre.rects.size() && sharpen.rects.front().w >= 8);
    CHECK(wipe.rects.size() == centre.rects.size() && wipe.rects.front().y == 0 && wipe.rects.back().y > 20);
    CHECK(reveal_of("center") == Reveal::Center && reveal_of("") == Reveal::Dissolve && reveal_of("x") == Reveal::Dissolve);
    std::string bytes;
    CHECK(data_uri_bytes("data:image/jpeg;base64,/9j/4A==", bytes) && bytes == std::string("\xff\xd8\xff\xe0"));
    CHECK(!data_uri_bytes("https://x/y.jpg", bytes));
    CHECK(cover_source("https://i.scdn.co/image/ab67") && cover_source("/rest/items/A/state") &&
          cover_source("data:image/png;base64,AAAA") && !cover_source("") && !cover_source("UNDEF") &&
          !cover_source("//x"));
  }

  Engine e(rec, cb);
  e.release_wait(0);   // releases act at once (the swipe check: its own test)
  CHECK(e.load_design(read("boards/nspanel/nextion/display-manifest-eu.json").c_str(), read("boards/nspanel/nextion/weather-icons.json").c_str()));
  e.set_config(read("test/host/data/office.json"));
  e.tick(1791196439);  // Mon 5 Oct 2026 12:33 local

  // start: home
  e.start();
  CHECK(rec.has("page 0"));
  CHECK(rec.has_prefix("th_accent=") && rec.has_prefix("th_text="));  // the theme in the display's globals,
  CHECK(rec.out.front().rfind("th_", 0) == 0);                         // before the page loads
  rec.out.clear();
  e.on_message(loaded("home"));
  CHECK(rec.has("vis 255,1"));
  CHECK(rec.pos("busy_tm.en=0") == 0);                               // the busy indicator stays away,
  CHECK(rec.pos("vis busy3,0", 1) > rec.pos("vis 255,1"));             // also after all is shown again
  CHECK(rec.pos("vis busy3,0", 1) < rec.pos("ref_star"));
  CHECK(rec.has("row1_n4.txt=\"23\""));        // the number rounded (no decimals asked for), in the
  CHECK(rec.has("row1_u4.txt=\"°C\""));         // field as wide (26 px: four half digits), its unit right after it
  CHECK(rec.has("vis row1_n4,1") && rec.has("vis row1_u4,1"));
  CHECK(rec.has("vis row1_u1,0") && rec.has("vis row1,0"));
  CHECK(rec.pos("vis row1,0") < rec.pos("vis row1_n4,1"));   // hidden first: it would paint over
  CHECK(!rec.has_prefix("row1.txt="));
  CHECK(rec.has("clock.txt=\"12:33\""));
  // outdoors as weather apps show it: degrees only, the sign in the number's size and colour (its wide field)
  CHECK(rec.has("outdoor.txt=\"17°\"") && rec.has("vis outdoor,1") && rec.has("vis outdoor_n4,0"));
  // today's low and high: "↓9° ↑19°", the low in a field as wide as it (21 px: three half digits),
  // the high's arrow and value right after it
  CHECK(rec.has("low3.txt=\"9°\"") && rec.has("vis low3,1") && rec.has("vis low_arrow,1"));
  CHECK(rec.has("high3.txt=\"19°\"") && rec.has("vis high_arrow3,1") && rec.has("vis high3,1"));
  CHECK(rec.has("vis high0,0") && rec.pos("vis high0,0") < rec.pos("vis high3,1"));   // hidden first
  CHECK(rec.has("date.txt=\"Mo, 5. Okt. 26\""));
  CHECK(!rec.has_prefix("date.pco="));                    // coloured by the page (th_dim)
  CHECK(!rec.has_prefix("info1_right.pco="));
  CHECK(rec.has(pic_cmd("weather_pic", "aurora", "weather_home", "rain")));  // 10d -> rain
  CHECK(rec.has("relay1_text.txt=\"Light\""));
  CHECK(rec.has("info1_tag.txt=\"92\""));             // tram line
  {  // too long for its field: cut, with "..."
    const std::string main = rec.with_prefix("info1_text.txt=\"Central St");
    CHECK(main.size() > 4 && main.compare(main.size() - 4, 4, "...\"") == 0);
    CHECK(main.find("Old Town") == std::string::npos);
  }
  CHECK(rec.has_prefix("shortcut3_icon.txt="));           // guest wifi shortcut (always shown)
  if (g_failures) rec.dump();

  {  // what counts as on: the states "on" names (a player's String item says PLAYING)
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *sc = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "shortcuts"), 0);
    cJSON_AddStringToObject(sc, "on", "PLAYING");
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("officeSpeakerState", "PLAYING", "String");
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has_prefix("shortcut1_icon.txt=") && !rec.has("shortcut1_icon.txt=\"\""));   // shown
    set_state("officeSpeakerState", "STOPPED", "String");
    e.on_state("officeSpeakerState");
    CHECK(rec.has("shortcut1_icon.txt=\"\""));                                          // hidden again
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // several icons are shown in turn while the indicator shows, every "interval" ms
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *sc = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "shortcuts"), 0);
    cJSON_ReplaceItemInObject(sc, "icon", cJSON_Parse("[\"equalizer\", \"poll\"]"));
    cJSON_AddNumberToObject(sc, "interval", 400);
    cJSON_AddStringToObject(sc, "on", "PLAYING");
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("officeSpeakerState", "PLAYING", "String");
    e.on_message(loaded("home"));
    const std::string eq = "shortcut1_icon.txt=\"" + nextion::quote(icon_glyph("equalizer")) + "\"",
                      poll = "shortcut1_icon.txt=\"" + nextion::quote(icon_glyph("poll")) + "\"";
    CHECK(e.animating() && rec.has(eq));
    rec.out.clear();
    e.animate(1000);   // starts its clock
    e.animate(1399);
    CHECK(rec.out.empty());
    e.animate(1400);
    CHECK(rec.has(poll));
    e.animate(1800);
    CHECK(rec.has(eq));
    set_state("officeSpeakerState", "STOPPED", "String");
    e.on_state("officeSpeakerState");
    CHECK(!e.animating() && rec.has("shortcut1_icon.txt=\"\""));
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // a shortcut with an item opens its page only while the item is on; one without always
    e.on_message(loaded("home"));
    set_state("officeSpeakerState", "OFF", "OnOff");
    e.on_state("officeSpeakerState");
    e.on_message(event("home", "shortcut1", "release"));
    CHECK(e.current() == "home");                       // off: not followed
    set_state("officeSpeakerState", "ON", "OnOff");
    e.on_state("officeSpeakerState");
    e.on_message(event("home", "shortcut1", "release"));
    CHECK(e.current() == "speaker");
    {  // with "open_when_off" it opens while the item is off too
      e.on_message(loaded("home"));
      set_state("officeSpeakerState", "OFF", "OnOff");
      e.on_state("officeSpeakerState");
      cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
      cJSON *sc = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "shortcuts"), 0);
      cJSON_AddTrueToObject(sc, "open_when_off");
      char *text = cJSON_PrintUnformatted(doc);
      e.set_config(text);
      cJSON_free(text);
      cJSON_Delete(doc);
      e.on_message(loaded("home"));
      e.on_message(event("home", "shortcut1", "release"));
      CHECK(e.current() == "speaker");
      e.set_config(read("test/host/data/office.json"));
    }
    // any text too wide for its field is cut (the manifest's "fit", Engine::txt), not only home's
    set_state("officeSpeakerCurrentTitle",
              "Ein sehr langer Titel, der nie und nimmer in die Zeile passt, auch nicht klein", "String");
    set_state("officeSpeakerCurrentArtist", "Kurz", "String");
    rec.out.clear();
    e.on_message(loaded("media"));
    {
      // what plays fills the page; no favourites page: no star
      const std::string track = rec.with_prefix("track.txt=\"Ein sehr");
      CHECK(track.size() > 4 && track.compare(track.size() - 4, 4, "...\"") == 0);
      CHECK(rec.has("artist.txt=\"Kurz\""));
      CHECK(rec.has("vis track,1") && rec.has("vis play_hs,1") && rec.has("vis fav_pic,0"));
      // play/pause: a disc in the accent drawn by rows (its widest: 13 rows across the middle), the triangle on it
      // in rows (paused)
      CHECK(rec.has_prefix("fill 183,246,85,13,") && !rec.has_prefix("cirs "));
    }
    e.on_message(event("media", "close", "release"));
    e.on_message(loaded("home"));
    e.on_message(event("home", "shortcut3", "release"));
    CHECK(e.current() == "guest-wifi");                 // no item: always
    rec.out.clear();
    e.on_message(loaded("qr"));
    // the QR page: the title, the caption, the code drawn by the firmware in its area (boards/nspanel/nextion/display-manifest-eu.json
    // "boxes"): first its white card, then the modules in the theme's accent, inside the card
    CHECK(rec.has("title.txt=\"Guest WiFi\"") && rec.has("caption1.txt=\"\""));
    {
      std::vector<std::array<int, 5>> fills;
      for (const auto &l : rec.out) {
        std::array<int, 5> f{};
        if (std::sscanf(l.c_str(), "fill %d,%d,%d,%d,%d", &f[0], &f[1], &f[2], &f[3], &f[4]) == 5)
          fills.push_back(f);
      }
      CHECK(fills.size() > 50 && fills.size() < 600);
      const auto card = fills.at(0);   // the middle of the card, its corners' rows after it
      CHECK(card[4] == 65535 && card[0] >= 127 && card[0] + card[2] <= 127 + 196);
      const auto module = fills.at(1 + 2 * 6);
      CHECK(module[4] != 65535 && module[0] >= card[0] + 6 && module[0] + module[2] <= card[0] + card[2] - 6);
      rec.out.clear();
      e.on_state("officeSpeakerState");                 // a state of no concern: the code is not drawn again
      CHECK(!rec.has_prefix("fill "));
    }
    e.on_message(event("qr", "close", "release"));
    e.on_message(loaded("home"));
    set_state("officeSpeakerState", "OFF", "OnOff");
  }

  {  // display.home_after: back to home once another page has not been touched that long
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *display = cJSON_GetObjectItem(doc, "display");
    if (!display)
      display = cJSON_AddObjectToObject(doc, "display");
    cJSON_AddNumberToObject(display, "home_after", 30);
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.idle(1000000);
    e.on_message(event("home", "shortcut3", "release"));   // guest wifi page, touched at 1000 s
    e.on_message(loaded("qr"));
    e.idle(1029000);
    CHECK(e.current() == "guest-wifi");                     // 29 s: stays
    e.idle(1030000);
    CHECK(e.current() == "home");                           // 30 s: home
    e.on_message(loaded("home"));
    e.idle(1049000);
    e.on_message(event("home", "shortcut3", "release"));     // the page again, touched at 1049 s
    e.on_message(loaded("qr"));
    e.idle(1049000 + 10000);
    e.on_message(event("qr", "qrcode_value", "press"));     // any touch there starts it again, at 1059 s
    e.idle(1059000 + 29000);
    CHECK(e.current() == "guest-wifi");                     // ... 29 s after it: stays
    e.idle(1059000 + 30000);
    CHECK(e.current() == "home");                           // 30 s after the last touch: home
    e.on_message(loaded("home"));
    e.idle(1059000 + 90000);
    CHECK(e.current() == "home");                           // on home nothing happens
    // without the setting: a minute
    e.set_config(read("test/host/data/office.json"));
    e.on_message(event("home", "shortcut3", "release"));
    e.on_message(loaded("qr"));
    e.idle(1149000 + 59000);
    CHECK(e.current() == "guest-wifi");
    e.idle(1149000 + 60000);
    CHECK(e.current() == "home");
    e.on_message(loaded("home"));
    // a player that plays stays; paused, it counts from then
    set_state("officeSpeakerState", "ON", "OnOff");
    e.on_state("officeSpeakerState");
    set_state("officeSpeakerControl", "PLAY", "Player");
    e.on_message(event("home", "shortcut1", "release"));
    e.on_message(loaded("media"));
    e.idle(1209000 + 300000);
    CHECK(e.current() == "speaker");
    set_state("officeSpeakerControl", "PAUSE", "Player");
    e.on_state("officeSpeakerControl");
    e.idle(1509000 + 59000);
    CHECK(e.current() == "speaker");
    e.idle(1509000 + 61000);
    CHECK(e.current() == "home");
    e.on_message(loaded("home"));
    // 0: never
    doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    display = cJSON_GetObjectItem(doc, "display");
    if (!display)
      display = cJSON_AddObjectToObject(doc, "display");
    cJSON_AddNumberToObject(display, "home_after", 0);
    text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(event("home", "shortcut3", "release"));
    e.on_message(loaded("qr"));
    e.idle(1149000 + 999000);
    CHECK(e.current() == "guest-wifi");
    e.on_message(event("qr", "close", "release"));
    e.on_message(loaded("home"));
  }

  {  // home is always the first of the swipe row, wherever an older configuration names it
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_ReplaceItemInObject(doc, "swipe", cJSON_Parse("[\"misc\", \"lights\", \"home\"]"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    CHECK(e.current() == "misc");                     // home, misc, lights
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));
    CHECK(e.current() == "home");
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "right"));
    CHECK(e.current() == "lights");                   // round the row
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
  }

  {  // a 12-hour clock: AM/PM above the minutes, drawn again with the time, after it, refreshing paused
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *disp = cJSON_GetObjectItem(doc, "display");
    if (!disp) disp = cJSON_AddObjectToObject(doc, "display");
    cJSON_DeleteItemFromObject(disp, "time_format");
    cJSON_AddStringToObject(disp, "time_format", "%-I:%M %p");
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    CHECK(rec.has("vis ampm,1"));
    rec.out.clear();
    e.tick(1791196439 + 120);   // the next minute: 12:35 PM
    const int stop = rec.pos("ref_stop"), time = rec.pos("clock.txt=\"12:35\""), pm = rec.pos("ampm.txt=\"PM\""),
              star = rec.pos("ref_star");
    CHECK(stop >= 0 && stop < time && time < pm && pm < star);
    if (g_failures) rec.dump();
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // the display restarted (the link down, then up again): nothing sent meanwhile; once it is up, the
     // theme and the page asked for, once
    e.link_down();
    rec.out.clear();
    e.on_state("officeTemperature");
    e.tick(1791196439 + 30);
    e.set_status(PanelStatus{});
    CHECK(rec.out.empty());
    e.link_up();
    CHECK(rec.has_prefix("th_accent=") && rec.has_prefix("bg_pic="));
    CHECK(std::count_if(rec.out.begin(), rec.out.end(), [](const std::string &c) { return c.rfind("page ", 0) == 0; }) == 1);
    CHECK(rec.has("page 0"));   // home
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has("vis 255,1"));
    if (g_failures) rec.dump();
  }

  {  // the display rewritten (or restarting): nothing drawn on whatever it shows meanwhile
    e.display_away();
    rec.out.clear();
    for (const char *item : {"nextDepartureLine", "officeTemperature"})
      e.on_state(item);
    e.tick(1791196439 + 60);
    CHECK(rec.out.empty());
    e.on_message(loaded("home"));   // not asked for: home after a restart
    CHECK(rec.has("vis 255,1"));
    if (g_failures) rec.dump();
  }

  // swipe left on home -> next page in the row: "lights" (button page)
  rec.out.clear();
  e.on_message(event("home", "swipe", "left"));
  CHECK(rec.has(page_cmd("buttons")));
  CHECK(e.current() == "lights");
  rec.out.clear();
  e.on_message(loaded("buttons"));
  CHECK(rec.has("title.txt=\"Office and bedroom lamp\""));
  CHECK(rec.has("tile1_name.txt=\"Desk\""));
  CHECK(rec.has("tile1_value.txt=\"Aus\""));
  CHECK(rec.has(pic_cmd("tile1_pic", "aurora", "button1", "off")));  // inactive
  CHECK(rec.has("vis tile3_pic,0"));                   // empty slot hidden
  // the buttons appear together, after the title: shown while refreshing is paused
  CHECK(rec.pos("vis title,1") >= 0 && rec.pos("vis title,1") < rec.pos("ref_stop"));
  CHECK(rec.pos("ref_stop") < rec.pos("vis tile1_pic,1") && rec.pos("vis tile1_pic,1") < rec.pos("ref_star"));
  CHECK(!rec.has("vis tile3_pic,1"));
  CHECK(rec.has_prefix("dots.txt="));

  // a button toggles its item
  e.on_message(event("buttons", "tile1", "release"));
  CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("DeskLamp_Switch"),
                                                                std::string("ON")));  // was OFF
  // ... and the state coming back redraws it
  rec.out.clear();
  set_state("DeskLamp_Switch", "ON", "OnOff");
  e.on_state("DeskLamp_Switch");
  CHECK(rec.has("tile1_value.txt=\"An\""));
  CHECK(rec.has(pic_cmd("tile1_pic", "aurora", "button1", "on")));
  CHECK(rec.out.front() == "ref_stop" && rec.out.back() == "ref_star");   // the tile and its fields in one go
  // unchanged values are not resent
  rec.out.clear();
  e.on_state("DeskLamp_Switch");
  CHECK(rec.out.empty());

  // a button opens a page; close goes back
  rec.out.clear();
  e.on_message(event("buttons", "tile2", "release"));
  CHECK(rec.has(page_cmd("light")) && e.current() == "desk-lamp");
  e.on_message(loaded("light"));
  CHECK(rec.has("title.txt=\"Desk lamp\""));
  CHECK(rec.has("vis side1_pic,0") && rec.has("vis slider,1"));   // only a brightness: no tiles to choose a view
  CHECK(rec.has("vis power_pic,1"));                                // but its on / off
  rec.out.clear();
  const auto value = [](const char *name, int v) {
    nextion::Message m;
    m.kind = nextion::Message::Kind::Value;
    m.name = name;
    m.value = v;
    return m;
  };
  e.on_message(value("cslu", 40));                        // the slider let go
  CHECK(commands.back() == std::make_pair(std::string("DeskLamp_Brightness"), std::string("40")));
  CHECK(rec.has("value.txt=\"40 %\""));
  // the lamp fading reports where it is on the way: not shown on the slider while the finger is on it, nor
  // just after -- then what the item says
  e.clock(5000000);
  e.on_message(value("cslu", 40));
  rec.out.clear();
  set_state("DeskLamp_Brightness", "25", "Percent", nullptr, 25, true);
  e.on_state("DeskLamp_Brightness");
  CHECK(!rec.has_prefix("slider.val=") && !rec.has_prefix("value.txt="));
  set_state("DeskLamp_Brightness", "39", "Percent", nullptr, 39, true);   // the lamp's 39 for 40
  e.on_state("DeskLamp_Brightness");
  e.idle(5000000 + 2500);
  CHECK(!rec.has_prefix("slider.val=") && rec.has("value.txt=\"39 %\""));   // the handle stays: nothing drawn anew
  // a lamp that did not follow: once the hold is over the slider shows where it is
  e.on_message(loaded("light"));
  e.clock(6000000);
  e.on_message(value("cslu", 80));
  set_state("DeskLamp_Brightness", "25", "Percent", nullptr, 25, true);
  e.on_state("DeskLamp_Brightness");
  rec.out.clear();
  e.idle(6000000 + 2500);
  CHECK(rec.has("slider.val=25"));
  e.on_message(event("light", "close", "release"));
  CHECK(rec.has(page_cmd("buttons")) && e.current() == "lights");

  // swipe right from the first row page returns home
  e.on_message(loaded("buttons"));
  e.on_message(event("buttons", "swipe", "right"));
  CHECK(e.current() == "home");

  // cover page: the position large and in words, the cover drawn, buttons and the drawing send commands
  e.on_message(loaded("home"));
  e.on_message(event("home", "swipe", "left"));
  e.on_message(event("buttons", "swipe", "left"));  // plants-covers
  CHECK(e.current() == "plants-covers");
  e.on_message(loaded("buttons"));
  rec.out.clear();
  e.on_message(event("buttons", "tile4", "release"));
  CHECK(e.current() == "cover-left");
  e.on_message(loaded("cover"));
  CHECK(rec.has("value.txt=\"30 %\"") && rec.has("state.txt=\"geschlossen\""));   // closed this far
  CHECK(rec.has_prefix("fill 18,48,142,"));                     // the shutter drawn in its window (draw_hs, inside the frame)
  const size_t sent = commands.size();
  e.on_message(event("cover", "stop_cover", "press"));       // touching: nothing yet
  CHECK(commands.size() == sent);
  e.on_message(event("cover", "stop_cover", "release"));     // letting go
  CHECK(commands.back() == std::make_pair(std::string("Blind_Left"), std::string("STOP")));
  e.on_message(value("cdrw", 80 * 1000 + 106));            // the finger on the drawing, a quarter down: only shown
  CHECK(commands.back().second == "STOP" && rec.has("value.txt=\"25 %\""));
  e.on_message(value("cdru", 80 * 1000 + 106));            // let go there
  CHECK(commands.back().second == "25");
  e.on_message(event("cover", "close_cover", "release"));
  CHECK(commands.back().second == "DOWN");
  // a jump straight to the end (the binding's echo of DOWN, a device's target): shown only once it lasts
  e.idle(6050000);
  set_state("Blind_Left", "100", "Percent", nullptr, 100, true);
  rec.out.clear();
  e.on_state("Blind_Left");
  CHECK(!rec.has("value.txt=\"100 %\""));
  e.idle(6050000 + 1600);
  CHECK(!rec.has("value.txt=\"100 %\""));
  e.idle(6050000 + 2600);
  CHECK(rec.has("value.txt=\"100 %\""));
  set_state("Blind_Left", "30", "Percent", nullptr, 30, true);
  e.on_state("Blind_Left");
  // sent down: the device's first word (where it was, a point higher) does not turn it to "opening"
  e.idle(6060000);
  e.on_message(value("cdru", 80 * 1000 + 180));            // to about 55 %
  set_state("Blind_Left", "29", "Percent", nullptr, 29, true);
  rec.out.clear();
  e.on_state("Blind_Left");
  e.idle(6060000 + 2100);                                   // the drag over: which way it goes shows
  CHECK(!rec.has("moving.txt=\"fährt hoch\"") && rec.has("moving.txt=\"fährt runter\""));

  // entity page with colour rules
  e.on_message(event("cover", "close", "release"));
  e.on_message(loaded("buttons"));
  e.on_message(event("buttons", "swipe", "left"));  // misc
  e.on_message(loaded("buttons"));
  rec.out.clear();
  e.on_message(event("buttons", "tile5", "release"));  // PV
  e.on_message(loaded("entities"));
  CHECK(rec.has("title.txt=\"Solar power\""));
  CHECK(rec.has("row1_shortval.txt=\"-12 W\""));             // narrow: in the narrow field,
  CHECK(rec.has("vis row1_longtext,1") && rec.has("vis row1_text,0"));   // the label beside it the long one
  CHECK(rec.has("row1_shortval.pco=63488"));                // below 0 -> red
  // a value too wide for the narrow field: the wide value column and the label as long as it leaves
  set_state("PV_Production_ActivePowerTotal", "-123456789 W", "Quantity", "-123456789.5 W", -123456789.5, true);
  rec.out.clear();
  e.on_state("PV_Production_ActivePowerTotal");
  CHECK(rec.has("row1_value.txt=\"-123456789,5 W\"") && rec.has("vis row1_shortval,0") && rec.has("vis row1_value,1"));
  CHECK(rec.has("vis row1_longtext,0") && rec.has("vis row1_text,1"));
  CHECK(rec.pos("vis row1_shortval,0") < rec.pos("vis row1_value,1"));   // the one going first: no overpainting
  set_state("PV_Production_ActivePowerTotal", "-12 W", "Quantity", "-12 W", -12, true);
  e.on_state("PV_Production_ActivePowerTotal");

  // the hardware button without an item switches its relay
  e.hardware_button(0);
  CHECK(!relays.empty() && relays.back() == std::make_pair(0, true));

  // another background: the display's globals change before the page is shown again, which takes them
  // on loading (boards/nspanel/nextion/ohpanel_hmi.py background_globals) -- nothing coloured over after drawing
  std::string cfg = read("test/host/data/office.json");
  cfg.replace(cfg.find("\"aurora\""), 8, "\"candy\"");
  rec.out.clear();
  e.set_config(cfg);
  const std::string candy = std::to_string(background_pic("candy"));
  CHECK(rec.has("bg_pic=" + candy));
  CHECK(rec.has("bg_close=" + std::to_string(manifest_pic("candy", "close", "x"))));
  rec.out.clear();
  e.on_message(loaded("entities"));
  CHECK(!rec.has_prefix("entities.pic=") && !rec.has_prefix("row1_value.picc=") && !rec.has("ref 0"));
  // icons and tiles come in the candy background's variants
  e.on_message(event("entities", "close", "release"));
  rec.out.clear();
  e.on_message(loaded("buttons"));
  CHECK(rec.has(pic_cmd("tile5_pic", "candy", "button5", "on")));  // PV, always active

  // the info page: from the misc page (shown since the step above), on a page of its own -- eight cards, an
  // icon, a label and the value each
  CHECK(e.current() == "misc");
  e.on_message(event("buttons", "tile6", "release"));
  CHECK(e.current() == "info" && rec.has(page_cmd("system")));
  rec.out.clear();
  e.on_message(loaded("system"));
  CHECK(rec.has("title.txt=\"Panel-Info\""));
  CHECK(rec.has("card1_text.txt=\"Firmware\"") && rec.has("card1_value.txt=\"v1.2\"") && rec.has("vis card1_value,1"));
  CHECK(rec.has("card2_text.txt=\"Display\"") && rec.has("card2_value.txt=\"fc44925a\""));
  CHECK(rec.has("card3_value.txt=\"2 d 03:04\""));
  CHECK(rec.has("card4_text.txt=\"Speicher\"") && rec.has("card4_value.txt=\"145 KB frei\""));
  CHECK(rec.has("card5_value.txt=\"1,8 / 2,0 MB\""));
  CHECK(rec.has("card6_text.txt=\"WLAN\"") && rec.has("card6_value.txt=\"home\""));
  CHECK(rec.has(std::string("card6_badge.txt=\"") + icon_glyph("wifi-strength-3") + "\"") && rec.has("vis card6_badge,1"));
  CHECK(rec.has("card7_value.txt=\"192.168.1.20\""));
  CHECK(rec.has("card8_text.txt=\"openHAB · http 8080\"") && rec.has("card8_value.txt=\"openhab\""));
  CHECK(rec.has(std::string("card8_icon.txt=\"") + icon_glyph("ohpanel-openhab") + "\""));
  if (g_failures) rec.dump();
  rec.out.clear();
  e.tick(1791196440);  // a second later: nothing changed, nothing resent
  CHECK(!rec.has_prefix("card7_value"));
  outdated = true;  // the display runs another design: the display card says so
  e.tick(1791196441);
  CHECK(rec.has_prefix("card2_value.txt=\"Design veraltet") || rec.has_prefix("card2_line1.txt=\"Design"));
  outdated = false;
  // a WiFi name too long for its line: in two, broken between words, under the label set higher
  ssid = "FRITZ!Box 7590 Gastzugang Erdgeschoss";
  rec.out.clear();
  e.tick(1791196442);
  CHECK(rec.has("card6_top.txt=\"WLAN\"") && rec.has("vis card6_value,0") && rec.has("vis card6_line2,1"));
  CHECK(rec.has_prefix("card6_line1.txt=\"FRITZ!Box") && rec.has_prefix("card6_line2.txt=\""));
  if (g_failures) rec.dump();
  ssid = "home";

  // the entity page's styles, a design page each: the list (seven rows, a line between them), the cards (eight),
  // the rows on cards of their own
  for (const char *style : {"cards", "rows", "list"}) {
    std::string scfg = read("test/host/data/office.json");
    scfg.insert(scfg.find("\"title\": \"Solar power\""), std::string("\"style\": \"") + style + "\", ");
    e.set_config(scfg);
    e.on_message(event(e.current() == "info" ? "system" : "entities", "close", "release"));
    CHECK(e.current() == "misc");
    e.on_message(loaded("buttons"));
    rec.out.clear();
    e.on_message(event("buttons", "tile5", "release"));   // PV: seven rows
    const std::string design = std::string(style) == "list" ? "entities" : std::string("entities_") + style;
    CHECK(rec.has(page_cmd(design.c_str())));
    rec.out.clear();
    e.on_message(loaded(design));
    const std::string n = std::string(style) == "cards" ? "card" : "row";
    CHECK(rec.has(n + (std::string(style) == "cards" ? "1_text" : "1_longtext") + ".txt=\"Solar output\"") &&
          rec.has("vis " + n + (std::string(style) == "cards" ? "1_value,1" : "1_shortval,1")));   // "-12 W": narrow
    CHECK(rec.has("vis " + n + "1_icon,1"));
    if (std::string(style) == "cards") {
      CHECK(rec.has(pic_cmd("card1_pic", "aurora", "card1", "box")) && rec.has("vis card1_pic,1"));
      CHECK(rec.has("vis card8_pic,0") && rec.has("vis card8_value,0"));   // an eighth place, empty
    } else if (std::string(style) == "rows") {
      CHECK(rec.has(pic_cmd("row1_pic", "aurora", "rowcard1", "box")) && rec.has("vis row7_pic,1"));
      CHECK(rec.has("vis row1_wide,0"));
    } else {
      CHECK(rec.has("vis row1_line,1") && rec.has("vis row6_line,1") && !rec.has("vis row7_line,1"));
    }
    CHECK(rec.has("vis dots,0"));   // one sheet: no dot
    if (g_failures) rec.dump();
  }
  {  // an eighth row in the list: a second sheet, dots for the two at the bottom, swiped to from the right
    std::string scfg = read("test/host/data/office.json");
    const size_t pv = scfg.find("\"rows\": [", scfg.find("\"title\": \"Solar power\""));
    scfg.insert(pv + 9, "{\"item\": \"Extra_Item\", \"label\": \"Extra\"}, {\"item\": \"Day_Item\", \"wide\": true, \"value_color\": \"blue\"}, ");
    e.set_config(scfg);
    rec.out.clear();
    e.on_message(loaded("entities"));
    const std::string full = icon_glyph("circle-medium"), small = icon_glyph("circle-small");
    CHECK(rec.has("row1_longtext.txt=\"Extra\"") && rec.has("dots.txt=\"" + full + small + "\"") && rec.has("vis dots,1"));
    CHECK(rec.has("vis row7_line,0"));
    CHECK(rec.has("vis row2_wide,1") && rec.has("vis row2_text,0") && rec.has("vis row2_longtext,0"));   // a wide row
    CHECK(rec.has("vis row1_wide,0"));                                                                     // beside one that is not
    CHECK(rec.has("row2_wide.pco=1055"));   // its value in its own colour, no rule needed
    rec.out.clear();
    e.on_message(event("entities", "swipe", "up"));   // up and down: nothing
    CHECK(!rec.has_prefix("page "));
    e.on_message(event("entities", "swipe", "left"));
    CHECK(e.current() == "pv" && rec.has_prefix("page "));   // still the page, its second sheet
    rec.out.clear();
    e.on_message(loaded("entities"));
    CHECK(rec.has("dots.txt=\"" + small + full + "\"") && rec.has("vis row3_icon,0"));
    e.on_message(event("entities", "swipe", "left"));   // past the last: nothing
    e.on_message(event("entities", "swipe", "right"));
    rec.out.clear();
    e.on_message(loaded("entities"));
    CHECK(rec.has("row1_longtext.txt=\"Extra\""));
    if (g_failures) rec.dump();
  }
  {  // texts too long for their fields scroll where the page or the row says so -- the others end in "..."
    std::string scfg = read("test/host/data/office.json");
    const size_t pv = scfg.find("\"rows\": [", scfg.find("\"title\": \"Solar power\""));
    scfg.insert(pv + 9, "{\"item\": \"Long_Item\", \"label\": \"Ein sehr langer Name, der nicht in sein Feld passt\"}, "
                        "{\"item\": \"Long_Item\", \"label\": \"Ein sehr langer Name, der nicht in sein Feld passt\", "
                        "\"scroll\": false}, ");
    scfg.insert(scfg.find("\"title\": \"Solar power\""), "\"scroll\": true, ");
    set_state("Long_Item", "fd00::1234:5678:9abc:def0:1234", "String");
    e.set_config(scfg);
    rec.out.clear();
    e.on_message(loaded("entities"));
    // the first: the label scrolls (beside the wide value column, the value too long for the narrow one); the value
    // ends in "..." -- a page scrolls 10 texts at most, the display's limit
    CHECK(rec.has("vis row1_textscr,1") && rec.has("row1_textscr.en=1") && rec.has("vis row1_text,0"));
    CHECK(rec.has_prefix("row1_textscr.txt=\"Ein sehr langer Name, der nicht in sein Feld passt "));
    CHECK(rec.pos("vis row1_text,0") < rec.pos("vis row1_textscr,1"));   // the field going first
    CHECK(rec.has("vis row1_value,1") && rec.has_prefix("row1_value.txt=\"fd00::"));
    // the second says no: shortened
    CHECK(rec.has("vis row2_text,1") && rec.has_prefix("row2_text.txt=\"Ein sehr langer") && !rec.has("vis row2_textscr,1"));
    // the third (as before) fits: no scrolling
    CHECK(rec.has("vis row3_shortval,1") && !rec.has("vis row3_textscr,1"));
    if (g_failures) rec.dump();
  }
  {  // a group's members as the rows: asked for afresh when the page opens, drawn when they come, by label, with
     // the page's icon, colour and rules
    e.on_message(event("entities", "close", "release"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));   // misc -> plants-covers
    e.on_message(loaded("buttons"));
    members_asked.clear();
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "plants-indoor");
    rec.out.clear();
    e.on_message(loaded("entities_rows"));   // the example's style: rows
    CHECK(members_asked.size() >= 1 && members_asked.front() == std::make_pair(std::string("gPlantsIndoor"), true));
    CHECK(rec.has("title.txt=\"Plant sensors (indoor)\"") && rec.has("vis row1_icon,0") && rec.has("vis dots,0"));
    members["gPlantsIndoor"] = {{"Plant_Aloe", "Aloe", {}}, {"Plant_Ficus", "Ficus", {}}};   // by label (appoh::members)
    set_state("Plant_Aloe", "12", "Decimal", "12 %", 12, true);
    set_state("Plant_Ficus", "40", "Decimal", "40 %", 40, true);
    rec.out.clear();
    e.on_options("gPlantsIndoor");
    CHECK(rec.has("row1_longtext.txt=\"Aloe\"") && rec.has("row2_longtext.txt=\"Ficus\"") && !rec.has("vis row3_icon,1"));
    CHECK(rec.has(std::string("row1_icon.txt=\"") + icon_glyph("sprout") + "\""));
    CHECK(rec.has("row1_shortval.txt=\"12 %\"") && rec.has("row1_shortval.pco=21920"));   // no metadata, no threshold: green
    CHECK(!rec.has("row1_icon.pco=21920"));   // the rule's colour is the value's: the icon keeps its own (icon_color would change it)
    rec.out.clear();
    set_state("Plant_Ficus", "10", "Decimal", "10 %", 10, true);
    e.on_state("Plant_Ficus");
    CHECK(rec.has("row2_shortval.txt=\"10 %\""));
    members["gPlantsIndoor"].erase(members["gPlantsIndoor"].begin());   // a member gone
    rec.out.clear();
    e.on_options("gPlantsIndoor");
    CHECK(rec.has("row1_longtext.txt=\"Ficus\"") && rec.has("vis row2_icon,0"));
    // with "plant" metadata: the label from the template, sorted by "order" (none: last), "hidden": true left out,
    // the thresholds the member's own
    auto plant = [](const char *name, const char *location, const char *order, const char *hidden) {
      appoh::Metadata md{std::string(name) + " (long)", {{"room", location}, {"short_name", name}, {"warn", "14"}, {"critical", "8"}}};
      if (*order) md.config["order"] = order;
      if (*hidden) md.config["hidden"] = hidden;
      appoh::Member m{std::string("Mi_") + name, "Bodenfeuchtigkeit", {{"plant", md}}};
      return m;
    };
    members["gPlantsIndoor"] = {plant("Ficus", "Wohnz.", "2", ""), plant("Palme", "Arbeitsz.", "", ""),
                                plant("Hibiskus", "Wohnz.", "1", "false"), plant("Bambus", "Dacht.", "3", "true")};
    set_state("Mi_Hibiskus", "8", "Decimal", "8 %", 8, true);
    set_state("Mi_Ficus", "14", "Decimal", "14 %", 14, true);
    set_state("Mi_Palme", "UNDEF", "UnDef");
    rec.out.clear();
    e.on_options("gPlantsIndoor");
    CHECK(rec.has("row1_longtext.txt=\"Wohnz. > Hibiskus\"") && rec.has("row2_longtext.txt=\"Wohnz. > Ficus\""));
    CHECK(rec.has("row3_longtext.txt=\"Arbeitsz. > Palme\"") && !rec.has("vis row4_icon,1"));
    CHECK(rec.has("row1_shortval.pco=63488") && rec.has("row2_shortval.pco=65472"));   // <= critical, <= warn
    CHECK(rec.has("row3_shortval.txt=\"–\""));
    if (g_failures) rec.dump();
  }
  {  // the weather page: home's weather icon opens its first day; the days are sheets, swiped left and right
    e.on_message(event("entities_rows", "close", "release"));   // plants-indoor -> plants-covers
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));          // -> lights
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));          // -> home
    e.on_message(loaded("home"));
    CHECK(e.current() == "home");
    e.on_message(event("home", "weather", "release"));
    CHECK(e.current() == "weather");
    rec.out.clear();
    e.on_message(loaded("weather"));
    CHECK(rec.has_prefix("title.txt=\"Heute · "));            // no title of its own: today, then the date
    // with the condition in words: the high smaller, the low right after it, the condition under them
    CHECK(rec.has("high_line.txt=\"19°\"") && rec.has("vis high_line,1") && rec.has("vis high,0"));
    CHECK(rec.has("condition.txt=\"Light rain\"") && rec.has("vis condition,1"));
    CHECK(rec.has_prefix("vis low_arrow") && rec.has_prefix("low") && rec.has("vis low_arrow,0") && rec.has("vis low,0"));
    {
      const std::string low = rec.with_prefix("low");   // lowK.txt="9°", K the place after "19°"
      CHECK(low.find(".txt=\"9°\"") != std::string::npos && low != "low.txt=\"9°\"");
    }
    CHECK(rec.has(pic_cmd("weather_pic", "aurora", "weather_page", "rain")) || rec.has_prefix("weather_pic.pic="));
    CHECK(rec.has("chip1_text.txt=\"Wind\"") && rec.has("vis chip1_pic,1") && rec.has("vis chip4_value,1"));
    CHECK(rec.has(pic_cmd("chip1_pic", "aurora", "wchip1", "box")));
    CHECK(rec.has("vis chip5_pic,0") && rec.has("vis chip6_icon,0"));   // four extras: two cards empty
    const std::string full = icon_glyph("circle-medium"), small = icon_glyph("circle-small");
    CHECK(rec.has("dots.txt=\"" + full + small + small + "\"") && rec.has("vis dots,1"));
    e.on_message(event("weather", "swipe", "left"));           // tomorrow
    CHECK(e.current() == "weather" && rec.has_prefix("page "));
    rec.out.clear();
    e.on_message(loaded("weather"));
    CHECK(rec.has_prefix("title.txt=\"Morgen · ") && rec.has("dots.txt=\"" + small + full + small + "\""));
    CHECK(rec.has("vis high,1") && rec.has("vis condition,0"));   // no condition that day: the high large, as before
    e.on_message(event("weather", "swipe", "left"));           // the day after: its weekday
    rec.out.clear();
    e.on_message(loaded("weather"));
    CHECK(!rec.has_prefix("title.txt=\"Morgen") && rec.has_prefix("title.txt=\""));
    if (g_failures) rec.dump();
    e.on_message(event("weather", "close", "release"));
    e.on_message(loaded("home"));
  }

  // a relay with an item: switches at once, tells openHAB, follows openHAB
  {
    std::string rcfg = read("test/host/data/office.json");
    const std::string desk = "\"label\": \"Desk lamp\"";
    rcfg.replace(rcfg.find(desk), desk.size(), desk + ", \"item\": \"Relay_Desk\"");
    e.set_config(rcfg);
    PanelStatus online;
    online.openhab = true;
    e.set_status(online);
    set_state("Relay_Desk", "OFF", "OnOff");
    relays.clear();
    e.hardware_button(1);
    CHECK(relays.size() == 1 && relays.back() == std::make_pair(1, true));
    CHECK(commands.back() == std::make_pair(std::string("Relay_Desk"), std::string("ON")));
    set_state("Relay_Desk", "ON", "OnOff");  // the echo changes nothing
    e.on_state("Relay_Desk");
    CHECK(relays.size() == 1);
    set_state("Relay_Desk", "OFF", "OnOff");  // openHAB switches it off
    e.on_state("Relay_Desk");
    CHECK(relays.size() == 2 && relays.back() == std::make_pair(1, false));

    // pressed while openHAB is away: the panel's state wins after the reconnect
    PanelStatus offline;
    e.set_status(offline);
    e.hardware_button(1);
    CHECK(relays.back() == std::make_pair(1, true));
    commands.clear();
    e.set_status(online);
    e.on_state("Relay_Desk");  // the snapshot after the reconnect still says OFF
    CHECK(relays.back() == std::make_pair(1, true));
    CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("Relay_Desk"), std::string("ON")));
    // on home the bar under it in the theme's accent while on (aurora: #4fc3f7), dark grey while off
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has("relay2_bar.bco=19998"));
    set_state("Relay_Desk", "OFF", "OnOff");
    e.on_state("Relay_Desk");
    CHECK(rec.has("relay2_bar.bco=10597"));

    // openHAB restarts: its items are NULL until restored -- the relay stays as it is, and openHAB is told
    set_state("Relay_Desk", "ON", "OnOff");
    e.on_state("Relay_Desk");
    CHECK(relays.back() == std::make_pair(1, true));
    e.set_status(offline);
    e.set_status(online);
    commands.clear();
    relays.clear();
    set_state("Relay_Desk", "NULL", "UnDef");
    e.on_state("Relay_Desk");
    CHECK(relays.empty());                                              // not switched off
    CHECK(commands.size() == 1 && commands.back() == std::make_pair(std::string("Relay_Desk"), std::string("ON")));
    e.on_state("Relay_Desk");
    CHECK(commands.size() == 1);                                        // told once
    set_state("Relay_Desk", "ON", "OnOff");                             // restored, or the command's echo
    e.on_state("Relay_Desk");
    CHECK(relays.empty());
    set_state("Relay_Desk", "OFF", "OnOff");                            // and followed again afterwards
    e.on_state("Relay_Desk");
    CHECK(relays.size() == 1 && relays.back() == std::make_pair(1, false));
    // NULL while openHAB is away (a stale snapshot): nothing sent, nothing switched
    e.set_status(offline);
    commands.clear();
    set_state("Relay_Desk", "UNDEF", "UnDef");
    e.on_state("Relay_Desk");
    CHECK(commands.empty() && relays.size() == 1);
    e.set_status(online);
    e.set_config(read("test/host/data/office.json"));
  }

  {  // value_style: a rule may set another icon, a colour and blinking -- icons and texts
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *home = cJSON_GetObjectItem(doc, "home");
    cJSON_DeleteItemFromObject(home, "status_icons");
    cJSON_AddItemToObject(home, "status_icons", cJSON_Parse(R"([{"item": "Window_Test", "icon": "window-closed-variant",
        "show": "always", "value_style": [{"equals": "OPEN", "icon": "window-open-variant", "color": "red",
                                           "blink": 500}]}])"));
    cJSON *line = cJSON_GetArrayItem(cJSON_GetObjectItem(home, "info"), 0);
    cJSON_AddItemToObject(cJSON_GetObjectItem(line, "right"), "value_style",
                          cJSON_Parse(R"([{"equals": "12:00", "color": "yellow", "blink": 250}])"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Window_Test", "CLOSED", "OpenClosed");
    set_state("nextDepartureTime", "12:00", "String");
    rec.out.clear();
    e.on_message(loaded("home"));
    const std::string closed = "top1.txt=\"" + icon_glyph("window-closed-variant") + "\"";
    const std::string open_ = "top1.txt=\"" + icon_glyph("window-open-variant") + "\"";
    CHECK(rec.has(closed) && !rec.has(open_));
    CHECK(rec.has("info1_right.txt=\"12:00\"") && rec.has("info1_right.pco=65472"));   // yellow (#f8f800)
    set_state("Window_Test", "OPEN", "OpenClosed");
    rec.out.clear();
    e.on_state("Window_Test");
    CHECK(rec.has(open_) && rec.has("top1.pco=63488"));                        // red
    e.animate(200000);
    rec.out.clear();
    e.animate(200250);
    CHECK(rec.has("info1_right.txt=\"\""));          // the time blinks (250 ms) ...
    e.animate(200500);
    CHECK(rec.has("top1.txt=\"\""));        // ... and the icon (500 ms)
    set_state("Window_Test", "CLOSED", "OpenClosed");
    rec.out.clear();
    e.on_state("Window_Test");
    CHECK(rec.has(closed));
    e.set_config(read("test/host/data/office.json"));
    set_state("nextDepartureTime", "21:53", "String");
    e.on_message(loaded("home"));
  }

  {  // a blinking status icon: the icon and nothing in turn, "blink" ms each
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *home = cJSON_GetObjectItem(doc, "home");
    cJSON_DeleteItemFromObject(home, "status_icons");
    cJSON_AddItemToObject(home, "status_icons", cJSON_Parse(R"([{"icon": "alert", "blink": 500}])"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    rec.out.clear();
    e.on_message(loaded("home"));
    const std::string alert = "top1.txt=\"" + icon_glyph("alert") + "\"";
    CHECK(rec.has(alert) && e.animating());
    e.animate(100000);
    rec.out.clear();
    e.animate(100500);
    CHECK(rec.has("top1.txt=\"\""));
    rec.out.clear();
    e.animate(101000);
    CHECK(rec.has(alert));
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // after a page loads its first texts are sent even when empty: what the field shows then is not
     // known (the display may have outlived a restart of the panel, its page not reloaded)
    PanelStatus up;
    up.wifi = true;
    e.set_status(up);
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has("wifi.txt=\"\""));
    rec.out.clear();
    e.set_status(up);
    CHECK(!rec.has_prefix("wifi.txt="));   // unchanged: not again
  }

  {  // a value row's rules: another icon, colour, blinking and format while one matches
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "values"), 0);
    cJSON_AddItemToObject(row, "value_style", cJSON_Parse(R"([{"above": 30, "color": "red"},
        {"above": 20, "color": "yellow", "blink": 500, "icon": "fire", "icon_color": "red", "format": "~%.0f%unit%"}])"));
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has("row1.txt=\"~23°C\""));   // the rule's format: the whole text, in the wide field
    CHECK(rec.has("row1.pco=65472"));
    CHECK(rec.has(std::string("row1_icon.txt=\"") + icon_glyph("fire") + "\""));
    CHECK(rec.has("row1_icon.pco=63488"));
    rec.out.clear();
    e.animate(100000);
    e.animate(100600);                                       // blinking
    CHECK(rec.has("row1.txt=\"\"") && rec.has("row1_icon.txt=\"\""));   // the icon with it
    rec.out.clear();
    set_state("Office_Temperature", "18.2 °C", "Quantity", nullptr, 18.2, true);
    e.on_state("Office_Temperature");    // none matches: as before, the page's colour
    CHECK(rec.has("row1_n4.txt=\"18\"") && rec.has("vis row1,0"));
    CHECK(rec.has(std::string("row1_icon.txt=\"") + icon_glyph("home-thermometer-outline") + "\""));
    rec.out.clear();
    e.animate(101200);
    CHECK(!rec.has_prefix("row1.txt"));              // its blinking stopped
    set_state("Office_Temperature", "23.2 °C", "Quantity", nullptr, 23.2, true);
    g_states["Office_Temperature"].unit = "°C";
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // the media page with a favourites page and a cover; the favourites; the volume page
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "speaker") {
        cJSON_AddStringToObject(pg, "favorites", "stations");
        cJSON_AddItemToObject(pg, "art", cJSON_Parse(R"({"item": "officeSpeakerArt"})"));
        cJSON_AddItemToObject(pg, "play", cJSON_Parse(R"({"item": "officeSpeakerPlayUri"})"));
        cJSON_AddItemToObject(pg, "uri", cJSON_Parse(R"({"item": "officeSpeakerTrackUri"})"));
      }
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "stations", "type": "media_favorites",
        "title": "Radio", "stations": {"item": "RadioStations"}})"));
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    set_state("officeSpeakerState", "ON", "OnOff");
    e.on_state("officeSpeakerState");
    e.on_message(event("home", "shortcut1", "release"));
    CHECK(e.current() == "speaker");
    set_state("officeSpeakerControl", "PAUSE", "Player");
    set_state("officeSpeakerCurrentTitle", "Nachrichten", "String");
    set_state("officeSpeakerCurrentArtist", "Studio One", "String");
    set_state("officeSpeakerVolume", "35", "Percent", nullptr, 35, true);
    rec.out.clear();
    covers.clear();
    e.on_message(loaded("media"));
    CHECK(rec.has("vis fav_pic,1") && rec.has("vis fav_hs,1"));
    CHECK(rec.has(std::string("fav_icon.txt=\"") + icon_glyph("star") + "\""));
    CHECK(rec.has("track.txt=\"Nachrichten\"") && rec.has("vis volume_pic,1") && !rec.has_prefix("m_vol.txt"));
    CHECK(covers.empty());                                  // no cover in the item
    // the star: the favourites, on the button pages' design
    rec.out.clear();
    e.on_message(event("media", "favorites", "release"));
    CHECK(e.current() == "stations" && rec.has(page_cmd("buttons")));
    rec.out.clear();
    asked.clear();
    e.on_message(loaded("buttons"));
    // the catalogue as known shows at once, and is asked for afresh (once, not with every sheet)
    CHECK(std::count(asked.begin(), asked.end(), std::make_pair(std::string("RadioStations"), true)) == 1);
    CHECK(rec.has("title.txt=\"Radio\"") && rec.has("tile1_text.txt=\"One\""));   // the short name
    CHECK(rec.has(std::string("tile1_center.txt=\"") + icon_glyph("radio") + "\"") && rec.has("vis tile1_center,1"));
    CHECK(rec.has(std::string("tile4_center.txt=\"") + icon_glyph("palm-tree") + "\""));
    CHECK(rec.has(std::string("tile2_center.txt=\"") + icon_glyph("newspaper") + "\""));
    CHECK(rec.has("vis tile1_icon,0") && rec.has("vis tile1_value,0"));
    CHECK(rec.has_prefix("dots.txt=\"") && !rec.has("dots.txt=\"\""));   // two sheets
    // Radio One' logo from idle(): the box cleared, the logo centred in it, the red laid on the dark tile
    CHECK(!rec.has_prefix("fill ") && e.pending());
    e.idle(1000);
    e.idle(1000 + 50);   // a part per idle(), a rest between them
    CHECK(rec.has("fill 18,65,84,62,10597") && rec.has("fill 59,95,2,1,65535") && rec.has("fill 59,96,2,1,37026"));
    // leaving the page while its logos come: nothing more drawn -- it would land on the next page
    e.on_message(event("buttons", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));
    e.on_message(loaded("buttons"));
    CHECK(e.pending());
    rec.out.clear();
    e.on_message(event("buttons", "swipe", "left"));
    e.idle(1100);
    CHECK(!rec.has_prefix("fill "));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "swipe", "right"));
    e.on_message(loaded("buttons"));
    e.idle(1200);
    rec.out.clear();
    e.on_message(event("buttons", "swipe", "left"));        // the second sheet
    CHECK(rec.has(page_cmd("buttons")));
    e.on_message(loaded("buttons"));
    CHECK(rec.has("tile1_text.txt=\"X4\"") && rec.has("vis tile3_hs,0"));
    e.on_message(event("buttons", "swipe", "right"));
    e.on_message(loaded("buttons"));
    // a tap: the station to the play item, back to the player, which shows its long name until it tells its own
    commands.clear();
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("officeSpeakerPlayUri"), std::string("uri:eins")));
    CHECK(e.current() == "speaker");
    rec.out.clear();
    e.on_message(loaded("media"));
    CHECK(rec.has("track.txt=\"Radio One live\"") && rec.has("artist.txt=\"\""));
    set_state("officeSpeakerTrackUri", "x-rincon-mp3radio://http://cdn.example/eins.mp3?token=1", "String");
    e.on_state("officeSpeakerTrackUri");
    // a stream telling only its address, later and long after: the station's name stays
    set_state("officeSpeakerCurrentTitle", "live.mp3?cid=123", "String");
    e.clock(9000000);
    e.on_state("officeSpeakerCurrentTitle");
    CHECK(!rec.has("track.txt=\"\"") && !rec.has_prefix("track.txt=\"live"));
    set_state("officeSpeakerCurrentTitle", "Die Sendung", "String");
    e.on_state("officeSpeakerCurrentTitle");
    CHECK(rec.has("track.txt=\"Die Sendung\"") && rec.has("artist.txt=\"Studio One\""));
    // learnt: the stream that came after the tap (its address without parameters) -- whenever it plays
    // without a title again, after a pause too, the station's name shows
    set_state("officeSpeakerCurrentTitle", "stream.mp3?cid=9", "String");
    set_state("officeSpeakerTrackUri", "x-rincon-mp3radio://http://cdn.example/eins.mp3?token=2", "String");
    set_state("officeSpeakerControl", "PAUSE", "Player");
    e.on_state("officeSpeakerControl");
    CHECK(rec.has("track.txt=\"Radio One live\""));
    set_state("officeSpeakerTrackUri", "x-sonos-spotify:spotify:track:abc?sid=9", "String");   // something else
    e.on_state("officeSpeakerTrackUri");
    CHECK(rec.has("track.txt=\"\""));
    // the same stream from another server: still that station
    set_state("officeSpeakerTrackUri", "x-rincon-mp3radio://http://other.example/eins.mp3?token=3", "String");
    e.on_state("officeSpeakerTrackUri");
    CHECK(rec.has("track.txt=\"Radio One live\""));
    e.on_message(event("media", "next", "release"));        // the player's own next
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerControl"), std::string("NEXT")));
    // a cover: asked for once, drawn over the art box from idle() -- from the middle outwards
    set_state("officeSpeakerArt", "https://i.example/cover.jpg", "String");
    e.on_state("officeSpeakerArt");
    CHECK(covers.size() == 1 && std::get<0>(covers[0]) == "https://i.example/cover.jpg" &&
          std::get<1>(covers[0]) == 70 && std::get<2>(covers[0]) == 2 && std::get<3>(covers[0]) == 18);
    e.on_state("officeSpeakerArt");
    CHECK(covers.size() == 1);
    std::vector<uint8_t> grey(70 * 70 * 3, 128);
    for (int i = 0; i < 3; i++)
      grey[(35 * 70 + 35) * 3 + i] = 255;                   // one white pixel in the middle
    rec.out.clear();
    e.on_cover("https://i.example/cover.jpg", std::make_shared<Cover>(cover_rects(grey.data(), 70, 2, 18)));
    CHECK(!rec.has_prefix("fill ") && e.pending());
    for (int t = 0; t < 20 && e.pending(); t++)
      e.idle(2000 + t * 50);
    CHECK(!e.pending());
    CHECK(rec.out.size() > 2 && rec.out.front() == "fill 94,130,2,2,65535");   // the middle first
    rec.out.clear();
    set_state("officeSpeakerControl", "PLAY", "Player");
    e.on_state("officeSpeakerControl");
    CHECK(!rec.has_prefix("art_icon.txt="));              // the icon under the cover stays as it is
    // playing: the disc again, two bars on it, exactly in its middle
    CHECK(rec.has_prefix("fill 183,246,85,13,") && rec.has("fill 212,236,9,32,4291") && rec.has("fill 229,236,9,32,4291"));
    // a picture the panel cannot read (a PNG, say): nothing drawn, the icon stays, not asked for again
    rec.out.clear();
    set_state("officeSpeakerArt", "https://i.example/logo.png", "String");
    e.on_state("officeSpeakerArt");
    CHECK(rec.has("ref art_pic") && rec.has("ref art_icon"));   // the old cover goes at once
    const size_t asked_before = covers.size();
    rec.out.clear();
    e.on_cover("https://i.example/logo.png", nullptr);
    for (int t = 0; t < 3; t++)
      e.idle(3000 + t * 50);
    CHECK(!rec.has_prefix("fill ") && !e.pending());
    e.on_state("officeSpeakerArt");
    CHECK(covers.size() == asked_before);
    set_state("officeSpeakerArt", "", "String");            // no cover at all: the icon, nothing to take away
    e.on_state("officeSpeakerArt");
    CHECK(!rec.has_prefix("fill "));
    // the volume: a page of its own over the media page
    rec.out.clear();
    e.on_message(event("media", "volume", "release"));
    CHECK(rec.has(page_cmd("volpage")) && e.current() == "speaker");
    rec.out.clear();
    e.on_message(loaded("volpage"));
    CHECK(rec.has("title.txt=\"Lautstärke\"") && rec.has("value.txt=\"35 %\"") && rec.has("slider.val=35"));
    CHECK(rec.has_prefix("mute.txt="));
    e.on_message(event("volpage", "plus", "release"));
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerVolume"), std::string("40")));
    nextion::Message slide;
    slide.kind = nextion::Message::Kind::Value;
    slide.name = "vols";                                    // moving: sent, every 300 ms at most
    slide.value = 47;
    e.clock(900000);
    e.on_message(slide);
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerVolume"), std::string("47")));
    const size_t before = commands.size();
    slide.value = 48;
    e.clock(900100);
    e.on_message(slide);
    CHECK(commands.size() == before);                      // too soon
    slide.name = "volu";                                    // let go: the last value
    slide.value = 50;
    e.on_message(slide);
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerVolume"), std::string("50")));
    e.on_message(event("volpage", "mute", "release"));
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerMute"), std::string("ON")));
    rec.out.clear();
    e.on_message(event("volpage", "close", "release"));     // back to the media page
    CHECK(rec.has(page_cmd("media")));
    e.on_message(loaded("media"));
    e.on_message(event("media", "close", "release"));
    e.on_message(loaded("home"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // two players sharing one favourites page: a tap plays on the player it came from -- opened from a
     // button page, on the page's "player"; the catalogues are fetched a while after openHAB connected
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *pages = cJSON_GetObjectItem(doc, "pages");
    for (cJSON *pg = pages->child; pg; pg = pg->next) {
      const std::string id = cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"));
      if (id == "speaker") {
        cJSON_AddStringToObject(pg, "favorites", "stations");
        cJSON_AddItemToObject(pg, "play", cJSON_Parse(R"({"item": "officeSpeakerPlayUri"})"));
      }
      if (id == "radio")   // its first tile opens the favourites
        cJSON_ReplaceItemInArray(cJSON_GetObjectItem(pg, "buttons"), 0,
                                 cJSON_Parse(R"({"label": "Radio", "icon": "star", "open": "stations"})"));
    }
    cJSON_AddItemToArray(pages, cJSON_Parse(R"({"id": "kitchen", "type": "media", "title": "Küche",
        "control": {"item": "kitchenControl"}, "favorites": "stations", "play": {"item": "kitchenPlayUri"}})"));
    cJSON_AddItemToArray(pages, cJSON_Parse(R"({"id": "stations", "type": "media_favorites", "title": "Radio",
        "stations": {"item": "RadioStations"}, "player": "kitchen"})"));
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    for (int k = 0; k < 6 && e.current() != "radio"; k++) {
      e.on_message(event(e.current() == "home" ? "home" : "buttons", "swipe", "left"));
      e.idle(3000000 + k * 10000);   // the fade (if any) and the page
      e.on_message(loaded(e.current() == "home" ? "home" : "buttons"));
    }
    CHECK(e.current() == "radio");
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "stations");
    e.on_message(loaded("buttons"));
    commands.clear();
    e.on_message(event("buttons", "tile2", "release"));
    CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("kitchenPlayUri"), std::string("uri:info")));
    CHECK(e.current() == "kitchen");
    e.on_message(event("media", "close", "release"));       // closing the player: where the favourites came from
    CHECK(e.current() == "radio");
    // and from the other player: on that one
    e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
    set_state("officeSpeakerState", "ON", "OnOff");
    e.on_state("officeSpeakerState");
    e.on_message(event("home", "shortcut1", "release"));
    CHECK(e.current() == "speaker");
    e.on_message(loaded("media"));
    e.on_message(event("media", "favorites", "release"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile2", "release"));
    CHECK(commands.back() == std::make_pair(std::string("officeSpeakerPlayUri"), std::string("uri:info")));
    CHECK(e.current() == "speaker");
    // openHAB connecting: the catalogue fetched afresh a while later, not at once
    PanelStatus st;
    st.openhab = false;
    e.set_status(st);
    asked.clear();
    e.clock(5000000);
    st.openhab = true;
    e.set_status(st);
    e.idle(5000000);
    CHECK(asked.empty());
    e.idle(5006000);
    CHECK(asked.size() == 1 && asked[0] == std::make_pair(std::string("RadioStations"), true));
    e.set_config(read("test/host/data/office.json"));
    e.on_message(loaded("home"));
  }

  {  // without "uri": a station learnt by the file name its stream tells as title; one name heard for two
     // stations (a broadcaster's "stream.mp3") shows none
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "speaker") {
        cJSON_AddStringToObject(pg, "favorites", "stations");
        cJSON_AddItemToObject(pg, "play", cJSON_Parse(R"({"item": "officeSpeakerPlayUri"})"));
      }
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "stations", "type": "media_favorites",
        "title": "Radio", "stations": {"item": "RadioStations"}})"));
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    for (int k = 0; k < 5 && e.current() != "home"; k++)
      e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
    set_state("officeSpeakerState", "ON", "OnOff");
    e.on_state("officeSpeakerState");
    set_state("officeSpeakerCurrentTitle", "Ein Lied", "String");
    const auto tap = [&](const char *button) {
      e.on_message(event("home", "shortcut1", "release"));
      e.on_message(loaded("media"));
      e.on_message(event("media", "favorites", "release"));
      e.on_message(loaded("buttons"));
      e.on_message(event("buttons", button, "release"));
      e.on_message(loaded("media"));
    };
    tap("tile4");   // Radio Four
    set_state("officeSpeakerCurrentTitle", "paradiso-berlin_mp3-192?ref=1", "String");
    e.on_state("officeSpeakerCurrentTitle");
    set_state("officeSpeakerCurrentTitle", "Ein Titel", "String");   // a real title: the hint goes
    e.on_state("officeSpeakerCurrentTitle");
    rec.out.clear();
    set_state("officeSpeakerCurrentTitle", "paradiso-berlin_mp3-192?ref=2", "String");   // again without one
    e.on_state("officeSpeakerCurrentTitle");
    CHECK(rec.has("track.txt=\"Radio Four\""));
    e.on_message(event("media", "close", "release"));
    e.on_message(loaded("home"));
    tap("tile1");   // Radio One: stream.mp3
    set_state("officeSpeakerCurrentTitle", "stream.mp3?cid=1", "String");
    e.on_state("officeSpeakerCurrentTitle");
    e.on_message(event("media", "close", "release"));
    e.on_message(loaded("home"));
    tap("tile2");   // News Radio: stream.mp3 too
    set_state("officeSpeakerCurrentTitle", "stream.mp3?cid=2", "String");
    e.on_state("officeSpeakerCurrentTitle");
    set_state("officeSpeakerCurrentTitle", "Nachrichten", "String");
    e.on_state("officeSpeakerCurrentTitle");
    rec.out.clear();
    set_state("officeSpeakerCurrentTitle", "stream.mp3?cid=3", "String");
    e.on_state("officeSpeakerCurrentTitle");
    CHECK(rec.has("track.txt=\"\""));   // which of the two: not known
    e.set_config(read("test/host/data/office.json"));
    e.on_message(event("media", "close", "release"));
    e.on_message(loaded("home"));
  }

  {  // a light with colour temperature (in K) and colour: its tiles choose the view; the wheel picks a colour
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "desk-lamp") {
        cJSON_AddItemToObject(pg, "color_temperature", cJSON_Parse(R"({"item": "DeskCt"})"));
        cJSON_AddItemToObject(pg, "color", cJSON_Parse(R"({"item": "DeskColor"})"));
      }
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    set_state("DeskCt", "4250 K", "Quantity", "4250 K", 4250, true);
    g_states["DeskCt"].unit = "K";
    set_state("DeskColor", "0,100,80", "HSB");
    for (int k = 0; k < 5 && e.current() != "home"; k++)
      e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));             // lights
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile2", "release"));
    CHECK(e.current() == "desk-lamp");
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("vis side1_pic,1") && rec.has("vis side3_pic,1") && rec.has("side2_text.txt=\"Farbtemp.\"") && rec.has_prefix("power_text.txt="));
    e.on_message(event("light", "side2", "release"));        // the colour temperature
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("vis temp_slider,1") && rec.has("vis slider,0") && rec.has("temp_slider.val=50"));   // 2000 .. 6500 K
    const auto value = [](const char *name, int v) {
      nextion::Message m;
      m.kind = nextion::Message::Kind::Value;
      m.name = name;
      m.value = v;
      return m;
    };
    e.on_message(value("ctpu", 100));
    CHECK(commands.back() == std::make_pair(std::string("DeskCt"), std::string("6500 K")));
    e.on_message(event("light", "side3", "release"));        // the colour wheel
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("vis wheel_pic,1") && rec.has("vis value,0"));
    CHECK(rec.has_prefix("cir ") && !rec.has_prefix("cirs "));   // the colour shown: a ring, and a disc (by rows)
    CHECK(rec.has("vis power_pic,0"));                               // in the on / off tile's place
    // a touch at the wheel's right edge: red, full saturation, the brightness kept
    set_state("DeskLamp_Brightness", "80", "Percent", nullptr, 80, true);   // on
    e.on_state("DeskLamp_Brightness");
    e.clock(20000000);
    e.on_message(value("cwhl", (100 + 250 - 1) * 1000 + (52 + 125)));
    CHECK(commands.back() == std::make_pair(std::string("DeskColor"), std::string("0,100,80")) ||
          commands.back().second.rfind("0,99,80", 0) == 0 || commands.back().second.rfind("359,", 0) == 0);
    e.clock(20000100);                                          // dragged on, 100 ms later: drawn, not sent yet
    e.on_message(value("cwhl", (100 + 125) * 1000 + (52 + 125 - 62)));   // straight up, half-way: 90 deg
    CHECK(commands.back().second != "90,50,80" && e.pending());
    e.clock(20000150);
    e.on_message(event("light", "wheel_hs", "press"));      // the finger still there: a touch
    e.idle(20000400);                                           // the last colour goes out once it may
    CHECK(commands.back().second == "90,50,80" && !e.pending());
    // a colour picked while the light is off: ON first, the colour once it is on, at the brightness it came with
    set_state("DeskLamp_Brightness", "0", "Percent", nullptr, 0, true);
    set_state("DeskColor", "90,50,0", "HSB");
    e.on_state("DeskColor");
    e.clock(20005000);
    e.on_message(value("cwhl", (100 + 250 - 1) * 1000 + (52 + 125)));
    CHECK(commands.back() == std::make_pair(std::string("DeskLamp_Brightness"), std::string("ON")));
    set_state("DeskLamp_Brightness", "54", "Percent", nullptr, 54, true);
    set_state("DeskColor", "90,50,54", "HSB");
    e.on_state("DeskColor");
    CHECK(commands.back().first == "DeskColor" && commands.back().second.rfind(",54") == commands.back().second.size() - 3 &&
          commands.back().second.rfind("0,", 0) == 0);
    // the on / off tile switches the light (no "switch": the brightness takes ON / OFF)
    set_state("DeskLamp_Brightness", "40", "Percent", nullptr, 40, true);
    e.on_state("DeskLamp_Brightness");
    e.on_message(event("light", "power", "release"));
    CHECK(commands.back() == std::make_pair(std::string("DeskLamp_Brightness"), std::string("OFF")));
    // and on again: ON (openHAB and the lamp keep the rest)
    set_state("DeskLamp_Brightness", "0", "Percent", nullptr, 0, true);
    e.on_state("DeskLamp_Brightness");
    e.on_message(event("light", "power", "release"));
    CHECK(commands.back() == std::make_pair(std::string("DeskLamp_Brightness"), std::string("ON")));
    // a colour temperature in percent, "reverse": 0 is cold, at the slider's top
    {
      cJSON *cfg = cJSON_Parse(read("test/host/data/office.json").c_str());
      for (cJSON *pg = cJSON_GetObjectItem(cfg, "pages")->child; pg; pg = pg->next)
        if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "desk-lamp") {
          cJSON_AddItemToObject(pg, "color_temperature", cJSON_Parse(R"({"item": "DeskCt"})"));
          cJSON_AddBoolToObject(pg, "reverse", true);
          cJSON_AddBoolToObject(pg, "live", false);   // sent only when let go
        }
      char *j = cJSON_PrintUnformatted(cfg);
      e.set_config(j);
      cJSON_free(j);
      cJSON_Delete(cfg);
    }
    set_state("DeskCt", "30", "Percent", nullptr, 30, true);
    g_states["DeskCt"].unit = "";
    e.on_message(event("light", "side2", "release"));
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("temp_slider.val=70") && rec.has("value.txt=\"30 %\""));
    const size_t before_live = commands.size();
    e.clock(9900000);
    e.on_message(value("ctps", 60));                          // moving: not sent ("live": false)
    CHECK(commands.size() == before_live);
    e.on_message(value("ctpu", 100));                         // the top: the coldest, 0 %
    CHECK(commands.back() == std::make_pair(std::string("DeskCt"), std::string("0")));
    // in mired (a Zigbee lamp: 250 .. 454, high is warm): the slider still warm at the bottom, Kelvin shown
    {
      cJSON *cfg = cJSON_Parse(read("test/host/data/office.json").c_str());
      for (cJSON *pg = cJSON_GetObjectItem(cfg, "pages")->child; pg; pg = pg->next)
        if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "desk-lamp") {
          cJSON_AddItemToObject(pg, "color_temperature", cJSON_Parse(R"({"item": "DeskCt"})"));
          cJSON_AddStringToObject(pg, "unit", "mired");
          cJSON_AddNumberToObject(pg, "min", 250);
          cJSON_AddNumberToObject(pg, "max", 454);
        }
      char *j = cJSON_PrintUnformatted(cfg);
      e.set_config(j);
      cJSON_free(j);
      cJSON_Delete(cfg);
    }
    set_state("DeskCt", "454", "Decimal", nullptr, 454, true);
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("temp_slider.val=0") && rec.has("value.txt=\"2200 K\""));   // the warmest: at the bottom
    e.on_message(value("ctpu", 100));
    CHECK(commands.back() == std::make_pair(std::string("DeskCt"), std::string("250")));
    // without ends in the page: the item's in openHAB (its stateDescription), the same lamp
    {
      cJSON *cfg = cJSON_Parse(read("test/host/data/office.json").c_str());
      for (cJSON *pg = cJSON_GetObjectItem(cfg, "pages")->child; pg; pg = pg->next)
        if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "desk-lamp") {
          cJSON_AddItemToObject(pg, "color_temperature", cJSON_Parse(R"({"item": "DeskCt"})"));
          cJSON_AddStringToObject(pg, "unit", "mired");
        }
      char *j = cJSON_PrintUnformatted(cfg);
      e.set_config(j);
      cJSON_free(j);
      cJSON_Delete(cfg);
    }
    limits["DeskCt"] = appoh::Limits{true, true, false, 250, 454};
    rec.out.clear();
    e.on_message(loaded("light"));
    CHECK(rec.has("temp_slider.val=0") && rec.has("value.txt=\"2200 K\""));
    e.on_message(value("ctpu", 100));
    CHECK(commands.back() == std::make_pair(std::string("DeskCt"), std::string("250")));
    limits.clear();   // none: the unit's usual ends (153 .. 500 mired)
    e.on_message(value("ctpu", 100));
    CHECK(commands.back() == std::make_pair(std::string("DeskCt"), std::string("153")));
    e.set_config(read("test/host/data/office.json"));
    e.on_message(event("light", "close", "release"));
    e.on_message(loaded("buttons"));
  }

  {  // a button with its icon in the middle: that field, no small line
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        cJSON_AddStringToObject(cJSON_GetArrayItem(cJSON_GetObjectItem(pg, "buttons"), 0), "layout", "center");
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    for (int k = 0; k < 5 && e.current() != "home"; k++)
      e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    CHECK(e.current() == "lights");
    rec.out.clear();
    e.on_message(loaded("buttons"));
    CHECK(rec.has_prefix("tile1_center.txt=\"") && rec.has("vis tile1_center,1") && rec.has("vis tile1_icon,0") &&
          rec.has("vis tile1_value,0"));
    CHECK(rec.has_prefix("tile2_icon.txt=\"") && rec.has("vis tile2_center,0"));
    e.set_config(read("test/host/data/office.json"));
    e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
  }

  {  // a swipe's end releases what the finger is on: that does nothing (in either order); a tap does
    e.release_wait(60);
    e.on_message(event("home", "swipe", "left"));          // to the lights page
    e.on_message(loaded("buttons"));
    commands.clear();
    int64_t t = 500000;
    e.clock(t);
    e.on_message(event("buttons", "swipe", "left"));        // swipe first, then the release
    e.on_message(event("buttons", "tile1", "release"));
    e.idle(t + 100);
    CHECK(commands.empty());
    e.on_message(loaded("buttons"));
    t += 2000;
    e.clock(t);
    e.on_message(event("buttons", "tile1", "release"));  // the release first, then the swipe
    e.clock(t + 5);
    e.on_message(event("buttons", "swipe", "right"));
    e.idle(t + 100);
    CHECK(commands.empty());
    e.on_message(loaded("buttons"));
    t += 2000;
    e.clock(t);
    e.on_message(event("buttons", "tile1", "release"));  // a tap: acts once the wait is over
    e.idle(t + 30);
    CHECK(commands.empty());
    e.idle(t + 70);
    CHECK(!commands.empty());
    e.release_wait(0);
    e.on_message(event("buttons", "close", "release"));
    e.on_message(loaded("home"));
  }

  {  // notifications: a card over the page, the page frozen under it, a touch closes it
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *display = cJSON_GetObjectItem(doc, "display");
    if (!display)
      display = cJSON_AddObjectToObject(doc, "display");
    cJSON_AddItemToObject(display, "notifications", cJSON_Parse(R"({"item": "Panel_Notify"})"));
    char *json = cJSON_PrintUnformatted(doc);
    e.set_config(json);
    cJSON_free(json);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    set_state("Panel_Notify", "warning|Old|from before", "String");
    rec.out.clear();
    e.on_state("Panel_Notify");                     // the state at the start: an old one
    CHECK(!rec.has_prefix("xstr"));
    set_state("Panel_Notify", "warning|Window open|The bathroom window has been open for half an hour now", "String");
    e.on_state("Panel_Notify");
    CHECK(rec.has_prefix("fill "));
    CHECK(rec.has_prefix("xstr ") && rec.has("sendxy=1") && rec.has("tsw 255,0"));
    bool yellow_title = false;
    for (const auto &c : rec.out)
      yellow_title = yellow_title || (c.find("xstr") == 0 && c.find(",4,65472,") != std::string::npos &&
                                      c.find("Window open") != std::string::npos);
    CHECK(yellow_title);
    rec.out.clear();
    e.tick(1791196441 + 7200);                       // the clock moves: not drawn over the card
    for (const auto &c : rec.out)
      CHECK(c.rfind("dim=", 0) == 0 || c.rfind("th_", 0) == 0 || c.rfind("bg_", 0) == 0);
    nextion::Message touch;
    touch.kind = nextion::Message::Kind::Point;
    touch.press = true;
    e.on_message(touch);
    CHECK(!rec.has("sendxy=0"));
    touch.press = false;
    e.on_message(touch);                             // released: closed, the page again
    CHECK(rec.has("sendxy=0") && rec.has(page_cmd("home")));
    e.on_message(loaded("home"));
    rec.out.clear();
    e.on_state("Panel_Notify");                      // the same state again: nothing
    CHECK(!rec.has_prefix("xstr"));
    set_state("Panel_Notify", "NULL", "UnDef");
    e.on_state("Panel_Notify");
    set_state("Panel_Notify", "warning|Window open|The bathroom window has been open for half an hour now", "String");
    e.on_state("Panel_Notify");                      // after NULL the same text shows again
    CHECK(rec.has_prefix("xstr "));
    bool cut = false;                                // a title too long for the card: shortened
    set_state("Panel_Notify", "alert|A title far too long for the card to hold in one line|Wörter", "String");
    e.on_state("Panel_Notify");
    e.on_message(loaded("home"));                    // (another card on screen: the page first)
    for (const auto &c : rec.out)
      cut = cut || (c.find(",4,63488,") != std::string::npos && c.find("...\"") != std::string::npos);
    CHECK(cut);
    rec.out.clear();
    set_state("Panel_Notify", "", "String");         // emptied: the card goes
    e.on_state("Panel_Notify");
    CHECK(rec.has("sendxy=0") && rec.has(page_cmd("home")));
    e.on_message(loaded("home"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // the hotspot opened by hand: the setup page comes, whatever was on, stays, and goes again
    PanelStatus setup;
    setup.hotspot = setup.setup = true;
    rec.out.clear();
    e.set_status(setup);
    CHECK(rec.has(page_cmd("boot")));
    rec.out.clear();
    e.on_message(loaded("boot"));
    CHECK(!rec.has(page_cmd("boot")));   // loaded, it stays: not asked for again (that flickered, over and over)
    e.hardware_button(0);       // a page change meanwhile: still the setup page
    e.tick(1791196441 + 3600);  // (back to home after a while too)
    CHECK(!rec.has(page_cmd("home")));
    rec.out.clear();
    e.set_status(PanelStatus{});  // joined again
    CHECK(rec.has(page_cmd("home")));
    e.on_message(loaded("home"));
  }

  {  // the NSPanel US on its side: the relays' labels upright beside its buttons -- the first's in the lower half,
     // read from the bottom up: its characters in the turned font (10) one over the other, each its advance in font 1
     // above the last
    Recorder r4;
    Engine g(r4, cb);
    CHECK(g.load_design(read("boards/nspanel/nextion/display-manifest-us-landscape.json").c_str(),
                        read("boards/nspanel/nextion/weather-icons.json").c_str()));
    g.set_config(read("test/host/data/office.json"));
    g.start();
    r4.out.clear();
    g.on_message(loaded("home"));
    std::vector<std::string> chars;
    for (const auto &c : r4.out)
      if (c.rfind("xstr ", 0) == 0 && c.find(",10,65535,") != std::string::npos)   // the first's, in white
        chars.push_back(c);
    CHECK(r4.has("vis relay1_text,0") && !r4.has_prefix("relay1_text.txt="));
    CHECK(chars.size() == 5 && chars[0].find("\"L\"") != std::string::npos && chars[4].find("\"t\"") != std::string::npos);
    const auto y = [](const std::string &c) { return std::stoi(c.substr(c.find(',') + 1)); };
    CHECK(chars.size() == 5 && y(chars[0]) + 16 < 172 + 132 && y(chars[1]) < y(chars[0]) && y(chars[4]) > 172);
  }

  {  // switched on with the panel: the display's boot page reported before anyone listened -- not
     // started yet, the panel asks for it itself, and draws on it once it loads
    Recorder r3;
    Engine f(r3, cb);
    f.release_wait(0);   // releases act at once (the swipe check: its own test)
    CHECK(f.load_design(read("boards/nspanel/nextion/display-manifest-eu.json").c_str(), read("boards/nspanel/nextion/weather-icons.json").c_str()));
    f.set_config(read("test/host/data/office.json"));
    PanelStatus up;
    up.hotspot = true;
    up.hotspot_ssid = "ohpanel-a1b2c3";
    up.ip = "192.168.4.1";
    f.set_status(up);
    CHECK(r3.has(page_cmd("boot")));
    r3.out.clear();
    f.set_status(up);                       // asked for once
    CHECK(!r3.has(page_cmd("boot")));
    f.on_message(loaded("boot"));
    CHECK(r3.has("status.txt=\"Einrichtung\"") && r3.has("device_value.txt=\"ohpanel-a1b2c3\""));
    r3.out.clear();
    f.set_status(up);                       // known now: not asked for again
    CHECK(!r3.has(page_cmd("boot")));
  }

  // hotspot mode: the boot page tells how to reach the setup page, with a QR code
  PanelStatus st;
  st.version = "v1";
  st.device = "ohpanel-a1b2c3";
  st.ip = "192.168.4.1";
  st.hotspot = true;
  st.hotspot_ssid = "ohpanel-a1b2c3";
  e.set_status(st);
  e.on_message(loaded("home"));   // nothing asked for on its way (that would leave the boot page)
  rec.out.clear();
  e.on_message(loaded("boot"));
  CHECK(rec.has("status.txt=\"Einrichtung\""));                 // the example is German
  CHECK(rec.has_prefix("logo_pic.pic="));                          // the logo on the background in use
  CHECK(rec.has("device_text.txt=\"WLAN\"") && rec.has("device_value.txt=\"ohpanel-a1b2c3\""));
  CHECK(rec.has("ip_value.txt=\"http://192.168.4.1\""));
  {
    // the hotspot's code drawn by the firmware (Engine::draw_code): a white card first -- 25 modules of 4 px
    // and its margin, the open network's short text -- then the dots
    std::array<int, 5> card{};
    CHECK(std::sscanf(rec.with_prefix("fill ").c_str(), "fill %d,%d,%d,%d,%d", &card[0], &card[1], &card[2], &card[3],
                      &card[4]) == 5);
    CHECK(card[4] == 65535 && card[2] == 25 * 4 + 2 * 5 && card[0] >= 450 - 29 - 110);
    CHECK(std::count_if(rec.out.begin(), rec.out.end(), [](const std::string &l) { return l.rfind("fill ", 0) == 0; }) > 100);
  }
  rec.out.clear();
  st.hotspot = false;
  e.set_status(st);
  CHECK(rec.has_prefix("page "));   // the code gone with the hotspot: the boot page anew

  {  // without an outdoor temperature in the configuration it does not show
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(doc, "home"), "outdoor_temperature");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(doc, "home"), "outdoor_low");  // a high alone: in front
    // a second value row, and the first info line's left field as a red badge
    cJSON *home = cJSON_GetObjectItem(doc, "home");
    cJSON_AddItemToArray(cJSON_GetObjectItem(home, "values"),
                         cJSON_Parse("{\"item\":\"nextDepartureLine\",\"icon\":\"water-percent\"}"));
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(home, "info"), 0), "left",
                              cJSON_Parse("{\"text\":\"92\",\"bg\":\"red\"}"));
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(home, "info"), 1), "left",
                              cJSON_Parse("{\"text\":\"\",\"bg\":\"blue\"}"));  // empty: no badge
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(event("boot", "close", "release"));
    rec.out.clear();
    e.on_message(loaded("home"));
    CHECK(rec.has("vis outdoor,0"));
    CHECK(rec.has("high0.txt=\"19°\"") && rec.has("vis high_arrow0,1") && rec.has("vis low_arrow,0"));
    CHECK(rec.has("vis row1_icon,1"));
    CHECK(rec.has("vis weather_pic,1"));
    CHECK(rec.has("vis row2,1"));
    CHECK(rec.has("row2.txt=\"92\""));
    // the red badge: the field on a picture of a rounded box, the text on it
    CHECK(rec.has("vis info1_tag,0"));
    CHECK(rec.has("vis info1_badge,1"));
    CHECK(rec.has(pic_cmd("info1_badge", "aurora", "badge1", "red")));
    CHECK(rec.has("info1_badge.txt=\"92\""));
    // the empty one with a background: no badge
    CHECK(rec.has("vis info2_badge,0"));
    CHECK(rec.has("vis info2_tag,1"));
  }

  {  // display.screen: brightness in use, idle, the night; numbers in step with openHAB items
    const auto with_screen = [](const char *screen) {
      cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
      cJSON *display = cJSON_GetObjectItem(doc, "display");
      if (!display)
        display = cJSON_AddObjectToObject(doc, "display");
      cJSON_DeleteItemFromObject(display, "screen");
      cJSON_AddItemToObject(display, "screen", cJSON_Parse(screen));
      char *text = cJSON_PrintUnformatted(doc);
      std::string out = text;
      cJSON_free(text);
      cJSON_Delete(doc);
      return out;
    };
    set_state("Panel_Bri", "70", "Percent", nullptr, 70, true);
    commands.clear();
    remembered.clear();
    int64_t t = 5000000;
    e.idle(t);
    e.set_config(with_screen(R"({"brightness": {"value": 80, "item": "Panel_Bri"}, "idle_brightness": 20,
                                 "idle_after": 30, "night": {"from": "23:00", "to": "06:00"}})"));
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "none"));   // a touch
    rec.out.clear();
    e.idle(t += 1000);
    CHECK(rec.has("dim=70"));                         // openHAB wins over the stored 80
    CHECK(commands.empty());                          // and is not told back
    e.idle(t += 5000);
    CHECK(remembered.size() == 1 && remembered[0] == std::make_pair(std::string("brightness"), 70));
    e.idle(t += 24000);                               // 30 s without a touch: idle
    CHECK(rec.has("dim=20") && e.brightness() == 20 && !e.asleep());
    e.on_message(event("home", "swipe", "none"));
    e.idle(t += 100);
    CHECK(e.brightness() == 70);
    // openHAB changes it
    set_state("Panel_Bri", "55", "Percent", nullptr, 55, true);
    e.on_state("Panel_Bri");
    e.idle(t += 100);
    CHECK(e.brightness() == 55);
    // the web interface changes it: it goes to the item
    e.set_config(with_screen(R"({"brightness": {"value": 90, "item": "Panel_Bri"}, "idle_brightness": 20,
                                 "idle_after": 30, "night": {"from": "23:00", "to": "06:00"}})"));
    CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("Panel_Bri"), std::string("90")));
    e.idle(t += 100);
    CHECK(e.brightness() == 90);
    // idle at 0 %: off, as at night -- the black page; a touch on it presses nothing and opens home
    e.set_config(with_screen(R"({"idle_brightness": 0, "idle_after": 30})"));
    e.on_message(event("home", "swipe", "none"));
    e.idle(t += 100);
    rec.out.clear();
    e.idle(t += 30000);
    CHECK(e.asleep() && rec.has("dim=0") && rec.has(page_cmd("blank")));
    e.on_message(loaded("blank"));
    commands.clear();
    e.on_message(event("home", "shortcut1", "release"));   // a press from the page before: nothing
    CHECK(commands.empty() && e.current() == "home");
    e.on_message(event("blank", "wake", "release"));
    rec.out.clear();
    e.idle(t += 100);
    CHECK(!e.asleep() && rec.has(page_cmd("home")) && rec.has("dim=85"));
    e.on_message(loaded("home"));
    // the night by the clock: off once idle, though idle_brightness is not 0
    e.set_config(with_screen(R"({"idle_after": 30, "night": {"from": "23:00", "to": "06:00"}})"));
    struct tm night = {};
    night.tm_year = 126; night.tm_mon = 9; night.tm_mday = 5; night.tm_hour = 23; night.tm_min = 30; night.tm_isdst = -1;
    e.tick(mktime(&night));
    e.on_message(event("home", "swipe", "none"));
    e.idle(t += 100);
    CHECK(!e.asleep() && e.brightness() == 85);        // in use: as bright as by day
    e.idle(t += 30000);
    CHECK(e.asleep() && e.brightness() == 0);
    e.on_message(loaded("blank"));
    e.idle(t += 100);
    CHECK(e.asleep());                                 // the black page loading is no touch: it stays off
    e.idle(t += 60000);
    CHECK(e.asleep());
    e.hardware_button(0);
    e.idle(t += 100);
    CHECK(e.asleep() && e.brightness() == 0);          // a front button switches, the screen stays off
    e.hardware_button(0);                              // (the relay back as it was)
    e.idle(t += 100);
    e.on_message(event("blank", "wake", "release"));
    e.idle(t += 100);
    CHECK(!e.asleep() && e.brightness() == 85);
    e.on_message(loaded("home"));
    struct tm day = night;
    day.tm_hour = 12;
    e.tick(mktime(&day));
    e.idle(t += 30000);
    CHECK(!e.asleep() && e.brightness() == 60);        // by day: the idle brightness (default 60)
    // the night by an item instead
    set_state("Panel_Night", "ON", "OnOff");
    e.set_config(with_screen(R"({"idle_after": 30, "night": {"item": "Panel_Night"}})"));
    e.on_message(event("home", "swipe", "none"));
    e.idle(t += 100);
    e.idle(t += 30000);
    CHECK(e.asleep());
    e.on_message(loaded("blank"));
    set_state("Panel_Night", "OFF", "OnOff");
    e.on_message(event("blank", "wake", "release"));
    e.idle(t += 100);
    e.on_message(loaded("home"));
    e.idle(t += 30000);
    CHECK(!e.asleep() && e.brightness() == 60);
    e.set_config(read("test/host/data/office.json"));
    e.on_message(event("home", "swipe", "none"));
    e.idle(t += 100);
  }

  {  // the boot page stays a few seconds once shown, even when the panel is ready sooner -- so it can be read
    Recorder r3;
    Engine b(r3, cb);
    CHECK(b.load_design(read("boards/nspanel/nextion/display-manifest-eu.json").c_str(), read("boards/nspanel/nextion/weather-icons.json").c_str()));
    b.set_config(read("test/host/data/office.json"));
    b.idle(10000);
    b.on_message(loaded("boot"));
    b.idle(11000);
    r3.out.clear();
    b.start();                              // ready after 1 s: home waits
    CHECK(!r3.has(page_cmd("home")));
    b.idle(12900);
    CHECK(!r3.has(page_cmd("home")));
    b.idle(13000);                          // 3 s since the boot page showed
    CHECK(r3.has(page_cmd("home")));
    if (g_failures) r3.dump();
  }

  {  // the settings page (the example is German): reached like any page; its sliders set display.screen
     // like the web interface
    static int restarts = 0;
    Callbacks cb2 = cb;
    cb2.restart = [] { restarts++; };
    std::vector<std::pair<std::string, int>> kept;
    cb2.remember = [&](const std::string &key, int value) { kept.push_back({key, value}); };
    std::vector<std::pair<std::string, std::string>> sent;
    cb2.command = [&](const std::string &item, const std::string &cmd) { sent.push_back({item, cmd}); };
    Recorder r2;
    Engine s(r2, cb2);
    s.release_wait(0);   // releases act at once (the swipe check: its own test)
    CHECK(s.load_design(read("boards/nspanel/nextion/display-manifest-eu.json").c_str(), read("boards/nspanel/nextion/weather-icons.json").c_str()));
    {  // the built-in settings page: not in the swipe row (no page dots) -- reached from home's first shortcut
      cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
      cJSON_ReplaceItemInObject(doc, "swipe", cJSON_Parse(R"(["settings"])"));
      cJSON *shortcut = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "shortcuts"), 0);
      cJSON_DeleteItemFromObject(shortcut, "open");
      cJSON_AddStringToObject(shortcut, "open", "settings");
      char *text = cJSON_PrintUnformatted(doc);
      s.set_config(text);
      cJSON_free(text);
      cJSON_Delete(doc);
    }
    s.start();
    s.on_message(loaded("home"));
    int64_t t = 1000;
    s.idle(t);
    s.on_message(event("home", "settings", "release"));   // home's gear does nothing (any more)
    CHECK(s.current() == "home");
    s.on_message(event("home", "swipe", "left"));   // "settings" in the row is skipped: no page to go to
    CHECK(s.current() == "home");
    s.on_message(event("home", "shortcut1", "release"));
    CHECK(s.current() == "settings" && r2.has(page_cmd("settings")));
    r2.out.clear();
    s.on_message(loaded("settings"));
    CHECK(r2.has("title.txt=\"Einstellungen\"") && r2.has("set1_slider.val=85") && r2.has("set1_value.txt=\"85 %\""));
    CHECK(r2.has("set3_slider.val=3") && r2.has("set3_value.txt=\"1 min\"") && r2.has("restart.txt=\"Neu starten\""));
    nextion::Message v;
    v.kind = nextion::Message::Kind::Value;
    v.name = "set0";
    v.value = 40;
    s.on_message(v);
    s.idle(t += 100);
    CHECK(s.brightness() == 40 && r2.has("set1_value.txt=\"40 %\""));
    v.name = "set2"; v.value = 0;
    s.on_message(v);
    CHECK(r2.has("set3_value.txt=\"nie\""));
    s.idle(t += 5000);
    CHECK(kept.size() == 2 && sent.empty());   // kept; no items linked, nothing sent
    // restart: the first tap asks again, the second restarts
    s.on_message(event("settings", "restart", "release"));
    CHECK(restarts == 0 && r2.has("restart.txt=\"Zum Neustart nochmal tippen\""));
    s.idle(t += 1000);
    s.on_message(event("settings", "restart", "release"));
    CHECK(restarts == 1);
    // while the display is away (an upload), nothing is sent: a command in the data breaks it
    s.display_away();
    r2.out.clear();
    s.idle(t += 600000);
    CHECK(!r2.has_prefix("dim="));
  }

  {  // a swipe to the next page with fading (page_fade): the light down in steps, the page, then up again
    Recorder r5;
    Engine g(r5, cb);
    g.release_wait(0);
    CHECK(g.load_design(read("boards/nspanel/nextion/display-manifest-eu.json").c_str(),
                        read("boards/nspanel/nextion/weather-icons.json").c_str()));
    std::string fading = read("test/host/data/office.json");
    fading.replace(fading.find("\"display\": {"), 12, "\"display\": {\"page_fade\": 80, ");   // 4 steps of 20 ms
    g.set_config(fading);
    g.tick(1791196439);
    g.start();
    g.on_message(loaded("home"));
    int64_t t = 100000;
    g.idle(t);                           // the brightness in use
    const int full = g.brightness();
    CHECK(full > 0);
    r5.out.clear();
    g.clock(t);
    g.on_message(event("home", "swipe", "left"));
    CHECK(r5.has("dim=" + std::to_string(full * 3 / 4)) && !r5.has_prefix("page "));   // the first step at once
    for (int k = 0; k < 3; k++)
      g.idle(t += 20);
    CHECK(r5.has("dim=0") && r5.has_prefix("page ") && g.pending());   // dark: the page
    const std::string page = r5.with_prefix("page ");
    r5.out.clear();
    std::string loaded_name;   // the design page that command shows
    for (const char *name : {"buttons", "entities", "cover", "light", "media", "weather", "qr", "home"})
      if (page_cmd(name) == page)
        loaded_name = name;
    CHECK(!loaded_name.empty());
    g.on_message(loaded(loaded_name));
    for (int k = 0; k < 4; k++)
      g.idle(t += 20);
    // up again to the brightness in use (the swipe was a touch: no longer the idle one), done
    CHECK(r5.has("dim=" + std::to_string(g.brightness())) && g.brightness() >= full && !g.pending());
  }

  {  // the thermostat page: the temperature, the target, heating; minus / plus, the ring and a mode send commands
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "thermo", "type": "thermostat",
        "title": "Kitchen", "item": "Kitchen_Setpoint", "current": {"item": "Kitchen_Temperature"},
        "heating": {"item": "Kitchen_Heating"},
        "mode": {"item": "Kitchen_Mode", "value_style": [{"equals": "auto", "locked": true}]},
        "modes": [{"label": "Auto", "value": "auto"}, {"label": "Manual", "value": "manual"},
                  {"label": "Off", "value": "off"}]})"));
    cJSON *lights = nullptr;   // its first tile opens it
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Heat", "icon": "thermostat", "open": "thermo"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Kitchen_Setpoint", "21 °C", "Quantity", nullptr, 21, true);
    g_states["Kitchen_Setpoint"].unit = "°C";
    set_state("Kitchen_Temperature", "20.54 °C", "Quantity", nullptr, 20.54, true);
    set_state("Kitchen_Heating", "ON", "OnOff");
    set_state("Kitchen_Mode", "manual", "String");
    limits["Kitchen_Setpoint"] = appoh::Limits{true, true, true, 10, 30, 0.5};
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "thermo");
    rec.out.clear();
    e.on_message(loaded("thermostat"));
    CHECK(rec.has("title.txt=\"Kitchen\"") && rec.has("now.txt=\" 20,5°\"") && rec.has("target.txt=\"Ziel 21,0 °C\""));
    const bool heats = std::any_of(rec.out.begin(), rec.out.end(), [](const std::string &c) {
      return c.rfind("xstr ", 0) == 0 && c.find("\"heizt\"") != std::string::npos; });
    CHECK(heats && rec.has("vis m31_hs,1") && !rec.has("vis m41_hs,1"));   // heating: its icon and word drawn
    CHECK(rec.has_prefix("cirs ") && rec.has_prefix("xstr "));   // the ring, the pills
    e.on_message(event("thermostat", "plus", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Kitchen_Setpoint"), std::string("21.5 °C")));
    CHECK(rec.has("target.txt=\"Ziel 21,5 °C\""));                                 // shown at once
    e.on_message(value("tdrw", 225 * 1000 + 160));                                     // inside the ring: nothing
    CHECK(commands.back().second == "21.5 °C");
    e.on_message(value("tdrw", 225 * 1000 + 48));                                      // its top: the middle, 20
    CHECK(rec.has("target.txt=\"Ziel 20,0 °C\"") && commands.back().second == "21.5 °C");
    e.on_message(value("tdru", 225 * 1000 + 48));
    CHECK(commands.back().second == "20 °C");
    // a swipe over the ring: along its top first (still the ring), then on sideways off it -- the target as it was,
    // nothing sent
    const size_t sent = commands.size();
    e.on_message(value("tdrw", 225 * 1000 + 48));
    e.on_message(value("tdrw", 245 * 1000 + 50));
    rec.out.clear();
    e.on_message(value("tdrw", 330 * 1000 + 52));
    e.on_message(value("tdru", 335 * 1000 + 52));
    CHECK(commands.size() == sent && rec.has("target.txt=\"Ziel 20,0 °C\""));
    e.on_message(event("thermostat", "mode3", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Kitchen_Mode"), std::string("off")));
    // a mode whose rule locks the target: no minus and plus, the ring not to be dragged
    set_state("Kitchen_Mode", "auto", "String");
    rec.out.clear();
    e.on_state("Kitchen_Mode");
    CHECK(rec.has("vis minus,0") && rec.has("vis plus_hs,0") && rec.has_prefix("picq "));
    const size_t before = commands.size();
    e.on_message(event("thermostat", "plus", "release"));
    e.on_message(value("tdru", 225 * 1000 + 48));
    CHECK(commands.size() == before);
    set_state("Kitchen_Mode", "manual", "String");
    rec.out.clear();
    e.on_state("Kitchen_Mode");
    CHECK(rec.has("vis minus,1") && rec.has("vis plus_hs,1"));
    e.on_message(event("thermostat", "close", "release"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a thermostat's on / off ("switch"): a pill; off, "off" for the target, no minus and plus, the ring not dragged
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "thermo", "type": "thermostat",
        "title": "Heat pump", "item": "Hp_Setpoint", "heating": {"item": "Hp_Heating"},
        "switch": {"item": "Hp_Power", "locked": true}})"));
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Heat", "icon": "thermostat", "open": "thermo"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Hp_Setpoint", "21", "Decimal", nullptr, 21, true);
    set_state("Hp_Heating", "ON", "OnOff");
    set_state("Hp_Power", "ON", "OnOff");
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    rec.out.clear();
    e.on_message(loaded("thermostat"));
    const auto drawn = [&](const std::string &word) {
      return std::any_of(rec.out.begin(), rec.out.end(), [&](const std::string &c) {
        return c.rfind("xstr ", 0) == 0 && c.find("\"" + word + "\"") != std::string::npos; });
    };
    CHECK(rec.has("vis power_hs,1") && drawn("An") && drawn("heizt"));   // its pill, on
    e.on_message(event("thermostat", "power", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Hp_Power"), std::string("OFF")));
    set_state("Hp_Power", "OFF", "OnOff");
    rec.out.clear();
    e.on_state("Hp_Power");
    CHECK(drawn("Aus") && rec.has("target.txt=\"Aus\"") && rec.has("vis minus,0") && !drawn("heizt"));
    const size_t before = commands.size();
    e.on_message(event("thermostat", "plus", "release"));
    e.on_message(value("tdru", 225 * 1000 + 48));
    CHECK(commands.size() == before);                              // the target not set while off
    e.on_message(event("thermostat", "power", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Hp_Power"), std::string("ON")));
    e.on_message(event("thermostat", "close", "release"));
    // without "locked": off is only its pill -- the target, minus and plus stay
    cJSON *doc2 = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc2, "pages"), cJSON_Parse(R"({"id": "thermo", "type": "thermostat",
        "title": "Heat pump", "item": "Hp_Setpoint", "switch": {"item": "Hp_Power", "label": "Boost", "icon": "fire"}})"));
    for (cJSON *pg = cJSON_GetObjectItem(doc2, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        cJSON_ReplaceItemInArray(cJSON_GetObjectItem(pg, "buttons"), 0,
                                 cJSON_Parse(R"({"label": "Heat", "icon": "thermostat", "open": "thermo"})"));
    text = cJSON_PrintUnformatted(doc2);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc2);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    rec.out.clear();
    e.on_message(loaded("thermostat"));
    CHECK(drawn("Boost") && !drawn("Aus") && rec.has_prefix("target.txt=\"Ziel") && !rec.has("vis minus,0"));   // its label
    const size_t sent2 = commands.size();
    e.on_message(event("thermostat", "plus", "release"));
    CHECK(commands.size() == sent2 + 1 && commands.back().first == "Hp_Setpoint");
    e.on_message(event("thermostat", "close", "release"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // the keypad page: the state over the code, the buttons; a button sends the code typed in its command, one
     // wanting a code says so without, a state that does not follow is "not accepted", a result item's new state shows
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "alarm", "type": "keypad",
        "title": "Alarm", "item": "Alarm_Command", "length": 4, "result": {"item": "Alarm_Message"},
        "state": {"item": "Alarm_State", "value_style": [
            {"equals": "DISARMED", "format": "Disarmed", "icon": "shield-off", "color": "green"},
            {"equals": "ARMED_AWAY", "format": "Armed away", "icon": "shield-lock", "color": "red"}]},
        "buttons": [{"label": "Away", "icon": "shield-lock", "command": "ARM_AWAY {code}", "when": "ARMED_AWAY"},
                    {"label": "Off", "icon": "shield-off", "command": "DISARM {code}", "when": "DISARMED"},
                    {"label": "Home", "item": "Alarm_Home", "command": "ON", "code": false}]})"));
    cJSON *lights = nullptr;   // its first tile opens it
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Alarm", "icon": "shield", "open": "alarm"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Alarm_State", "DISARMED", "String");
    set_state("Alarm_Message", "", "String");
    e.idle(30000000);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "alarm");
    rec.out.clear();
    e.on_message(loaded("keypad"));
    CHECK(rec.has("title.txt=\"Alarm\"") && rec.has("places.val=4") && rec.has("longest.val=4"));
    CHECK(rec.has("vis b31_hs,1") && rec.has("vis b33_hs,1") && !rec.has("vis b41_hs,1"));   // the set of three
    CHECK(rec.has("vis k10i,1") && rec.has("k12.txt=\"C\""));                             // backspace, 0, C
    const auto drawn = [&](const std::string &word, int colour) {
      return std::any_of(rec.out.begin(), rec.out.end(), [&](const std::string &c) {
        return c.rfind("xstr ", 0) == 0 && c.find("\"" + word + "\"") != std::string::npos &&
               c.find("," + std::to_string(colour) + ",") != std::string::npos; });
    };
    CHECK(drawn("Disarmed", 21920) && drawn("Away", 52857));   // the state in green; a pill not lit
    e.on_message(event("keypad", "key", "press"));
    e.on_message(event("keypad", "button1", "1234"));
    CHECK(commands.back() == std::make_pair(std::string("Alarm_Command"), std::string("ARM_AWAY 1234")));
    CHECK(rec.has("message.txt=\"Gesendet\"") && rec.has("vis message,1"));
    const size_t sent = commands.size();
    e.on_message(event("keypad", "button2", ""));                 // no code: it asks for one
    CHECK(commands.size() == sent && rec.has("message.txt=\"Code eingeben\""));
    e.on_message(event("keypad", "button3", ""));                 // one without a code, to its own item
    CHECK(commands.back() == std::make_pair(std::string("Alarm_Home"), std::string("ON")));
    e.on_message(event("keypad", "button1", "1111"));
    rec.out.clear();
    e.idle(30000000 + 7000);                                      // the state is still DISARMED: not accepted
    CHECK(rec.has("message.txt=\"Nicht angenommen\""));
    e.on_message(event("keypad", "button1", "1234"));
    set_state("Alarm_State", "ARMED_AWAY", "String");
    rec.out.clear();
    e.on_state("Alarm_State");
    CHECK(drawn("Armed away", 63488));                            // red now
    e.idle(30000000 + 8000);
    CHECK(!rec.has("message.txt=\"Nicht angenommen\""));          // it followed
    e.on_message(event("keypad", "button2", "4321"));
    set_state("Alarm_Message", "Window open", "String");
    rec.out.clear();
    e.idle(30000000 + 9000);
    CHECK(rec.has("message.txt=\"Window open\""));                // the result item's word
    CHECK(rec.has_suffix_of("fill ", ",10597"));                  // ... and the button no longer lit
    e.on_message(event("keypad", "key", "release"));             // a key at 39 s, then none
    rec.out.clear();
    e.idle(30000000 + 9000 + 19000);
    CHECK(!rec.has("code.txt=\"\""));
    e.idle(30000000 + 9000 + 20000);                              // 20 s without one: the code goes
    CHECK(rec.has("code.txt=\"\""));
    e.on_message(event("keypad", "close", "release"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // keypad buttons by the state ("show_when"): disarmed the two to arm, armed only "Off" -- the page anew when the
     // set changes, a button's event then meaning the buttons that show
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "alarm", "type": "keypad",
        "title": "Alarm", "item": "Alarm_Command", "state": {"item": "Alarm_State"},
        "buttons": [{"label": "Away", "command": "ARM_AWAY {code}", "show_when": "DISARMED"},
                    {"label": "Home", "command": "ARM_HOME {code}", "show_when": "DISARMED"},
                    {"label": "Off", "command": "DISARM {code}", "show_when": ["ARMED_AWAY", "ARMED_HOME"]}]})"));
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Alarm", "icon": "shield", "open": "alarm"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Alarm_State", "DISARMED", "String");
    e.idle(40000000);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    rec.out.clear();
    e.on_message(loaded("keypad"));
    CHECK(rec.has("vis b21_hs,1") && rec.has("vis b22_hs,1") && !rec.has("vis b11_hs,1"));
    e.on_message(event("keypad", "button2", "1234"));
    CHECK(commands.back() == std::make_pair(std::string("Alarm_Command"), std::string("ARM_HOME 1234")));
    set_state("Alarm_State", "ARMED_HOME", "String");
    rec.out.clear();
    e.on_state("Alarm_State");                                    // another set: swapped in place, no page anew
    CHECK(!rec.has_prefix("page ") && rec.has("vis b21_hs,0") && rec.has("vis b22_hs,0") && rec.has("vis b11_hs,1"));
    CHECK(rec.has_prefix("picq ") && rec.has_prefix("fill "));    // the buttons' place back, the new pill
    e.on_message(event("keypad", "button1", "1234"));             // the one that shows: "Off"
    CHECK(commands.back() == std::make_pair(std::string("Alarm_Command"), std::string("DISARM 1234")));
    e.on_message(event("keypad", "close", "release"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a keypad toggle button: its own item switched without a code (ON / OFF by its state), lit while that is on
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "alarm", "type": "keypad",
        "title": "Alarm", "item": "Alarm_Command",
        "buttons": [{"label": "Away", "command": "ARM_AWAY {code}"},
                    {"label": "Light", "icon": "lightbulb", "action": "toggle", "item": "Hall_Light"}]})"));
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Alarm", "icon": "shield", "open": "alarm"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Hall_Light", "OFF", "OnOff");
    e.idle(50000000);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    rec.out.clear();
    e.on_message(loaded("keypad"));
    const auto lit = [&](const std::string &word) {   // its label drawn white: on the accent
      return std::any_of(rec.out.begin(), rec.out.end(), [&](const std::string &c) {
        return c.rfind("xstr ", 0) == 0 && c.find("\"" + word + "\"") != std::string::npos && c.find(",65535,") != std::string::npos; });
    };
    CHECK(!lit("Light"));
    const size_t sent = commands.size();
    e.on_message(event("keypad", "button2", ""));                 // no code needed
    CHECK(commands.size() == sent + 1 && commands.back() == std::make_pair(std::string("Hall_Light"), std::string("ON")));
    CHECK(!rec.has("message.txt=\"Code eingeben\""));
    set_state("Hall_Light", "ON", "OnOff");
    rec.out.clear();
    e.on_state("Hall_Light");
    CHECK(lit("Light"));                                          // lit by its item
    e.on_message(event("keypad", "button2", "12"));               // a code typed: not sent, the item switched
    CHECK(commands.back() == std::make_pair(std::string("Hall_Light"), std::string("OFF")));
    e.on_message(event("keypad", "close", "release"));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a swipe row without home: its first page is the start page (no close cross there, "back" goes to it); a list's
     // sheets come before the next page, and swiping back lands on the list's last sheet
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_ReplaceItemInObject(doc, "swipe", cJSON_Parse(R"(["lights", "schedule", "guest-wifi"])"));
    char *text = cJSON_PrintUnformatted(doc);
    rec.out.clear();
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    CHECK(e.current() == "lights");                               // home was shown: the start page now
    CHECK(rec.has("no_close=1"));                                 // its cross hidden as it loads
    e.on_message(loaded("buttons"));
    CHECK(rec.has("vis close,0") && rec.has("vis close_hs,0"));   // nothing to go back to
    e.on_message(event("buttons", "swipe", "left"));
    CHECK(e.current() == "schedule");
    e.on_message(loaded("entities"));
    // its sheets first (16 rows: more than one), then the next page
    int sheets = 1;
    for (; sheets < 10; sheets++) {
      e.on_message(event("entities", "swipe", "left"));
      if (e.current() != "schedule")
        break;
      e.on_message(loaded("entities"));
    }
    CHECK(sheets > 1 && e.current() == "guest-wifi");
    e.on_message(loaded("qr"));
    e.on_message(event("qr", "swipe", "right"));                  // back: on the list's last sheet, so as many to go
    for (int k = 1; k < sheets; k++) {
      CHECK(e.current() == "schedule");
      e.on_message(loaded("entities"));
      e.on_message(event("entities", "swipe", "right"));
    }
    CHECK(e.current() == "schedule");
    e.on_message(loaded("entities"));
    e.on_message(event("entities", "swipe", "right"));
    CHECK(e.current() == "lights");
    rec.out.clear();
    e.on_message(loaded("buttons"));
    {   // a dot for every sheet of the row's pages: this one first, the list's, the QR code's
      std::string want = icon_glyph("circle-medium");
      for (int k = 0; k < sheets + 1; k++)
        want += icon_glyph("circle-small");
      CHECK(rec.has("dots.txt=\"" + want + "\""));
    }
    e.on_message(event("buttons", "swipe", "right"));             // round to the last
    CHECK(e.current() == "guest-wifi");
    e.on_message(loaded("qr"));
    rec.out.clear();
    e.on_message(event("qr", "close", "release"));                // the cross: to the start page
    CHECK(e.current() == "lights" && !rec.has("no_close=0"));
    e.on_message(loaded("buttons"));
    {   // home in the row but not first, shown: a configuration saved keeps it
      cJSON *d2 = cJSON_Parse(read("test/host/data/office.json").c_str());
      cJSON_ReplaceItemInObject(d2, "swipe", cJSON_Parse(R"(["lights", "home"])"));
      char *t2 = cJSON_PrintUnformatted(d2);
      e.set_config(t2);
      e.on_message(event("buttons", "swipe", "left"));
      CHECK(e.current() == "home");
      e.on_message(loaded("home"));
      e.set_config(t2);
      CHECK(e.current() == "home");
      cJSON_free(t2);
      cJSON_Delete(d2);
    }
    e.set_config(read("test/host/data/office.json"));
    CHECK(e.current() == "home");                                 // home in the row: it stays
  }

  {  // a cover group page with presets (the group's metadata): the pills in the first row's place, three members a
     // sheet under them; a preset's position goes to the whole group, a member's row still opens it
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"),
                         cJSON_Parse(R"({"id": "front", "type": "cover", "title": "Front", "group": "gFront"})"));
    cJSON *lights = nullptr;   // its first tile opens it
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Front", "icon": "roller-shade", "open": "front"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    members["gFront"] = {{"Blind_1", "Blind 1", {}}, {"Blind_2", "Blind 2", {}}, {"Blind_3", "Blind 3", {}},
                         {"Blind_4", "Blind 4", {}}, {"Blind_5", "Blind 5", {}}};
    metadata["gFront"] = {{"cover", appoh::Metadata{" ", {{"presets", "50 %=50, 75 %=75"}}}}};
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "front");
    rec.out.clear();
    e.on_message(loaded("covers"));
    CHECK(rec.has("vis row1_hs,0") && rec.has("vis row2_hs,1") && rec.has("row2_text.txt=\"Blind 1\"") &&
          rec.has("row4_text.txt=\"Blind 3\""));                       // three members a sheet, from the second row
    CHECK(rec.has("vis preset1_hs,1") && rec.has("vis preset2_hs,1") && rec.has("vis preset3_hs,0"));
    CHECK(rec.has("vis dots,1"));                                        // five members, three a sheet: two sheets
    const auto last_dots = [&] {   // what the dots say last: the sheets, not the swipe row's (none: not in it)
      std::string last;
      for (const std::string &c : rec.out)
        if (c.rfind("dots.txt=", 0) == 0)
          last = c;
      return last;
    };
    CHECK(!last_dots().empty() && last_dots() != "dots.txt=\"\"");
    const bool pill = std::any_of(rec.out.begin(), rec.out.end(), [](const std::string &c) {
      return c.rfind("xstr ", 0) == 0 && c.find("\"75 %\"") != std::string::npos; });
    CHECK(pill);                                                         // the label drawn on its pill
    e.on_message(event("covers", "preset2", "release"));
    CHECK(!commands.empty() && commands.back() == std::make_pair(std::string("gFront"), std::string("75")));
    e.on_message(event("covers", "swipe", "left"));                      // the next sheet: members 4 and 5
    rec.out.clear();
    e.on_message(loaded("covers"));
    CHECK(rec.has("row2_text.txt=\"Blind 4\"") && rec.has("vis row4_hs,0"));
    e.on_message(event("covers", "row2", "release"));
    CHECK(e.current() == "@cover");                                      // Blind 4 as a cover page of its own
    // the metadata only after the page opened: first four a sheet (two sheets), then the page anew with the presets
    e.on_message(event("cover", "close", "release"));                    // back to the group page
    e.on_message(loaded("covers"));
    e.on_message(event("covers", "close", "release"));
    metadata.erase("gFront");
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    rec.out.clear();
    e.on_message(loaded("covers"));
    CHECK(rec.has("vis row1_hs,1") && rec.has("vis dots,1"));
    metadata["gFront"] = {{"cover", appoh::Metadata{" ", {{"presets", "50 %=50, 75 %=75"}}}}};
    rec.out.clear();
    e.on_options("gFront");
    CHECK(rec.has(page_cmd("covers")));
    rec.out.clear();
    e.on_message(loaded("covers"));
    CHECK(rec.has("vis row1_hs,0") && rec.has("vis preset1_hs,1") && rec.has("vis dots,1"));
    metadata.erase("gFront");
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a separate command item ("commands": "item"): up / stop / down go there, its words as they read (also
     // reversed), the position to the item; on a group page each member's metadata names its own
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "curtain", "type": "cover",
        "item": "Curtain_Dimmer", "style": "curtain", "reverse": true,
        "commands": {"item": "Curtain_Cmd", "open": "open", "stop": "stop", "close": "close"}})"));
    cJSON_AddItemToArray(cJSON_GetObjectItem(doc, "pages"), cJSON_Parse(R"({"id": "curtains", "type": "cover",
        "title": "Curtains", "group": "gCurtains", "commands": {"open": "open", "stop": "stop", "close": "close"}})"));
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
                             cJSON_Parse(R"({"label": "Curtain", "icon": "curtains", "open": "curtain"})"));
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 1,
                             cJSON_Parse(R"({"label": "Curtains", "icon": "curtains", "open": "curtains"})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    set_state("Curtain_Dimmer", "40", "Percent", nullptr, 40, true);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    e.on_message(loaded("buttons"));
    e.on_message(event("buttons", "tile1", "release"));
    CHECK(e.current() == "curtain");
    e.on_message(loaded("cover"));
    e.on_message(event("cover", "open_cover", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Curtain_Cmd"), std::string("open")));   // not swapped
    e.on_message(event("cover", "stop_cover", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Curtain_Cmd"), std::string("stop")));
    e.on_message(event("cover", "close_cover", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Curtain_Cmd"), std::string("close")));
    e.on_message(value("cdru", 80 * 1000 + 106));            // the drawing: the position, to the item
    CHECK(commands.back().first == "Curtain_Dimmer");
    e.on_message(event("cover", "close", "release"));
    e.on_message(loaded("buttons"));

    members["gCurtains"] = {{"Curtain_A", "Curtain A", {{"cover", appoh::Metadata{" ", {{"command_item", "Curtain_A_Cmd"}}}}}},
                            {"Curtain_B", "Curtain B", {}}};
    e.on_message(event("buttons", "tile2", "release"));
    CHECK(e.current() == "curtains");
    e.on_message(loaded("covers"));
    commands.clear();
    e.on_message(event("covers", "all_up", "release"));       // the whole group: each member on its own
    CHECK(commands.size() == 2 && commands[0] == std::make_pair(std::string("Curtain_A_Cmd"), std::string("open")) &&
          commands[1] == std::make_pair(std::string("Curtain_B"), std::string("open")));
    e.on_message(event("covers", "row1_down", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Curtain_A_Cmd"), std::string("close")));
    e.on_message(event("covers", "row1", "release"));         // Curtain A as a page of its own: its command item
    CHECK(e.current() == "@cover");
    e.on_message(loaded("cover"));
    e.on_message(event("cover", "stop_cover", "release"));
    CHECK(commands.back() == std::make_pair(std::string("Curtain_A_Cmd"), std::string("stop")));
    members.erase("gCurtains");
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a field's room in bytes (the manifest's third number): a text that fits its width but not its bytes -- ü,
     // ł take two -- is cut at a character, with "...", not by the display in the middle of a letter
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
        cJSON_Parse(R"({"label": "Frühstück", "icon": "coffee", "layout": "center", "always_active": true})"));
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 1,
        cJSON_Parse(R"({"label": "ŁóżkoŚćŹżŃą", "icon": "bed", "layout": "center", "always_active": true})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    rec.out.clear();
    e.on_message(loaded("buttons"));
    CHECK(rec.has("tile1_text.txt=\"Frühstück\""));                    // 11 bytes: fits (the field: 20)
    std::string second;
    for (const std::string &c : rec.out)
      if (c.rfind("tile2_text.txt=\"", 0) == 0)
        second = c.substr(16, c.size() - 17);
    CHECK(second.size() <= 20 && second.size() >= 3 && second.compare(second.size() - 3, 3, "...") == 0);
    CHECK(second.rfind("Łóżko", 0) == 0);                               // whole letters before the dots
    e.set_config(read("test/host/data/office.json"));
  }

  {  // Cyrillic by its own widths (the manifest's "ranges"), not "?"'s: seven Ш are 14 bytes, within the field's
     // 20, but 98 px of the 80 -- as "?" they would be 42 and pass uncut
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON *lights = nullptr;
    for (cJSON *pg = cJSON_GetObjectItem(doc, "pages")->child; pg; pg = pg->next)
      if (std::string(cJSON_GetStringValue(cJSON_GetObjectItem(pg, "id"))) == "lights")
        lights = pg;
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(lights, "buttons"), 0,
        cJSON_Parse(R"({"label": "ШШШШШШШ", "icon": "coffee", "layout": "center", "always_active": true})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    e.on_message(loaded("home"));
    e.on_message(event("home", "swipe", "left"));
    rec.out.clear();
    e.on_message(loaded("buttons"));
    CHECK(!rec.has("tile1_text.txt=\"ШШШШШШШ\""));
    CHECK(rec.has("tile1_text.txt=\"ШШШШ...\"") || rec.has("tile1_text.txt=\"ШШШШШ...\""));
    e.set_config(read("test/host/data/office.json"));
  }

  {  // a date rule with a time of day turns at that time, without a new state: a bin shortcut yellow from 17:00 the
     // evening before (Engine::tick redraws what shows a date each minute)
    cJSON *doc = cJSON_Parse(read("test/host/data/office.json").c_str());
    cJSON_ReplaceItemInArray(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "home"), "shortcuts"), 1, cJSON_Parse(
        R"({"item": "BinDate", "icon": "delete", "show": "always", "value_style": [{"within_days": 1, "from": "17:00",
            "color": "yellow"}]})"));
    char *text = cJSON_PrintUnformatted(doc);
    e.set_config(text);
    cJSON_free(text);
    cJSON_Delete(doc);
    const time_t monday_1659 = 1791212340;   // Mon 5 Oct 2026, 16:59:00 local; the bin goes on Tuesday
    set_state("BinDate", "1791298800", "Decimal", nullptr, 1791298800, true);
    e.tick(monday_1659);
    e.on_message(loaded("home"));
    CHECK(rec.has_prefix("shortcut2_icon.txt=") && !rec.has("shortcut2_icon.pco=65472"));   // not yet
    rec.out.clear();
    e.tick(monday_1659 + 60);                                                                  // 17:00
    CHECK(rec.has("shortcut2_icon.pco=65472"));                                                // yellow now
    rec.out.clear();
    e.tick(monday_1659 + 120);                                                                 // 17:01: unchanged
    CHECK(!rec.has_prefix("shortcut2_icon"));
    e.set_config(read("test/host/data/office.json"));
  }

  if (g_failures) {
    std::fprintf(stderr, "%d engine check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("engine: all checks passed\n");
  return 0;
}
