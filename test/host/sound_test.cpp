/*
   Host test for the buzzer's tones: the names the schema takes, the ones the
   firmware plays (Sound.cpp) and the ones the web interface offers
   (www/index.html, TONES) are the same, and the notifications' defaults of the
   firmware (Format.h) are the web interface's (NOTE_TONES).
*/

#include "appdisplay/Format.h"
#include "sound/Sound.h"
#include "panel/Schema.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

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
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string list(const std::set<std::string> &s) {
  std::string out;
  for (const auto &x : s)
    out += (out.empty() ? "" : ", ") + x;
  return out;
}

void same(const char *what, const std::set<std::string> &got, const std::set<std::string> &want) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s: %s\n  the firmware's: %s\n", what, list(got).c_str(), list(want).c_str());
    g_failures++;
  }
}

}  // namespace

int main() {
  std::set<std::string> firmware;
  for (const auto &n : sound::tone_names()) {
    firmware.insert(n);
    CHECK(sound::tone_notes(n) && !sound::tone_notes(n)->empty());
  }
  CHECK(!sound::tone_notes("none"));

  // the schema: a tone it refuses lists the ones it takes ("must be one of: none, click, ...")
  std::vector<panel::Issue> issues;
  panel::validate(R"({"version":1,"display":{"sound":{"touch":"?"}}})", issues);
  std::set<std::string> schema;
  const std::string prefix = "must be one of: ";
  if (issues.size() == 1 && issues[0].message.rfind(prefix, 0) == 0) {
    std::stringstream ss(issues[0].message.substr(prefix.size()));
    for (std::string n; std::getline(ss, n, ',');)
      schema.insert(n.substr(n.find_first_not_of(' ')));
  }
  CHECK(schema.erase("none") == 1);
  same("the schema's tones", schema, firmware);

  // the web interface: const TONES = [['', 'none'], ['click', ...], ...]
  const std::string html = read("www/index.html");
  const size_t from = html.find("const TONES = ["), to = html.find("];", from);
  CHECK(from != std::string::npos && to != std::string::npos);
  std::set<std::string> web;
  const std::string tones = from == std::string::npos ? "" : html.substr(from, to - from);
  const std::regex entry(R"(\['([a-z-]*)',)");
  for (auto it = std::sregex_iterator(tones.begin(), tones.end(), entry); it != std::sregex_iterator(); ++it)
    web.insert((*it)[1]);
  CHECK(web.erase("") == 1);
  same("the web interface's tones", web, firmware);

  // the notifications' defaults: const NOTE_TONES = {info: 'rising', warning: 'falling', alert: 'alarm'};
  const appdisplay::SoundSettings defaults;
  const char *const levels[] = {"info", "warning", "alert"};
  for (int k = 0; k < 3; k++) {
    std::smatch m;
    const std::regex def(std::string("NOTE_TONES = \\{[^}]*") + levels[k] + ": '([a-z-]*)'");
    CHECK(std::regex_search(html, m, def) && m[1] == defaults.notify[k]);
    CHECK(firmware.count(defaults.notify[k]) == 1);
  }

  // melodies (RTTTL): a4 at 440 Hz, a quarter at b=120 500 ms, a tenth of it (at most 20 ms) a break at its end
  {
    std::vector<sound::Note> n;
    std::string error;
    CHECK(sound::melody_notes("test:d=4,o=4,b=120:a,8p,c5,2e.5", n, &error));
    CHECK(n.size() == 7 && n[0].hz == 440 && n[0].ms == 480 && n[1].hz == 0 && n[1].ms == 20);   // a, its break
    CHECK(n.size() == 7 && n[2].hz == 0 && n[2].ms == 250);                                       // 8p: no break
    CHECK(n.size() == 7 && n[3].hz == 523 && n[3].ms == 480);                                     // c5
    CHECK(n.size() == 7 && n[5].hz == 659 && n[5].ms == 1480 && n[6].ms == 20);                   // 2e.5: 1500 ms
    // the dot before the octave too, sharps, h for b, upper case and blanks; the defaults d=4, o=6, b=63
    CHECK(sound::melody_notes("X: d=8, b=240 : C#.5, H, P", n) && n.size() == 5 && n[0].hz == 554 &&
          n[0].ms == 187 - 18 && n[2].hz == 1976 && n[2].ms == 125 - 12 && n[4].hz == 0);
    CHECK(sound::melody_notes("::a", n) && n.size() == 2 && n[0].hz == 1760 && n[0].ms + n[1].ms == 952);
    CHECK(sound::melody_notes("d=4:a", n) && n.size() == 2);   // defaults without a name
    CHECK(sound::melody_notes("name:a,b", n) && n.size() == 4);  // a name without defaults
    // what is none: a note, a default, a length, an octave, nothing at all, too many notes
    CHECK(!sound::melody_notes("x:d=4:a,x", n, &error) && error.find("\"x\"") != std::string::npos);
    CHECK(!sound::melody_notes("x:q=4:a", n, &error) && error.find("q=4") != std::string::npos);
    CHECK(!sound::melody_notes("x:d=3:a", n));
    CHECK(!sound::melody_notes("x:b=10:a", n));
    CHECK(!sound::melody_notes("x::3a", n) && !sound::melody_notes("x::a9", n) &&
          !sound::melody_notes("x::a##", n) && !sound::melody_notes("x::a..", n));
    CHECK(!sound::melody_notes("x::", n, &error) && error == "no notes");
    CHECK(!sound::melody_notes("a:b:c:d", n));
    CHECK(!sound::melody_notes(std::string("x::a,") + '\0', n));
    std::string many = "x::";
    for (size_t k = 0; k <= sound::kMaxMelodyNotes; k++)
      many += "a,";
    CHECK(!sound::melody_notes(many, n, &error) && error.find("notes") != std::string::npos);
    // a tone's name or a melody
    CHECK(sound::sound_notes("beep", n) && n.size() == 1);
    CHECK(sound::sound_notes("x::a", n) && n.size() == 2);
    CHECK(!sound::sound_notes("bang", n, &error) && error == "no such tone");
    CHECK(sound::sound_notes("2a", n) && n.size() == 2);   // a single note is a melody too
    CHECK(!sound::sound_notes("x::a,y", n, &error) && error.find("\"y\"") != std::string::npos);
    // at most 400 notes and pauses, however many entries they make for the buzzer
    std::string pauses = "x::", quick = "x:d=32,b=900:";
    for (size_t k = 0; k < sound::kMaxMelodyNotes; k++) {
      pauses += "p,";
      quick += "a,";
    }
    CHECK(sound::melody_notes(pauses, n) && n.size() == sound::kMaxMelodyNotes);
    CHECK(sound::melody_notes(quick, n));
    CHECK(!sound::melody_notes(pauses + "p", n, &error) && error == "more than 400 notes");
    CHECK(!sound::melody_notes(quick + "a", n));
  }

  // the web interface's examples: const MELODIES = [['name', 'rtttl' + 'rtttl'], ...]; each plays
  {
    const size_t from = html.find("const MELODIES = ["), to = html.find("];", from);
    CHECK(from != std::string::npos && to != std::string::npos);
    const std::string list = from == std::string::npos ? "" : html.substr(from, to - from);
    const std::regex melody(R"(\['([^']+)', '[^']*', ('[^\]]+')\])");   // title, where from, the melody
    int count = 0;
    for (auto it = std::sregex_iterator(list.begin(), list.end(), melody); it != std::sregex_iterator(); ++it) {
      std::string rtttl;   // its pieces joined
      const std::string pieces = (*it)[2];
      const std::regex piece("'([^']*)'");
      for (auto p = std::sregex_iterator(pieces.begin(), pieces.end(), piece); p != std::sregex_iterator(); ++p)
        rtttl += (*p)[1];
      std::vector<sound::Note> n;
      std::string error;
      if (!sound::melody_notes(rtttl, n, &error)) {
        std::fprintf(stderr, "FAIL the example %s: %s\n", std::string((*it)[1]).c_str(), error.c_str());
        g_failures++;
      }
      count++;
    }
    CHECK(count >= 5);
  }

  // the web interface's parser (parseMelody) is held to this one's results: the cases, with what this one makes of
  // them, for test/host/melody_test.js
  {
    std::vector<std::string> cases = {
        "test:d=4,o=4,b=120:a,8p,c5,2e.5", "X: d=8, b=240 : C#.5, H, P", "::a", "d=4:a", "name:a,b", "x:d=4:a,x",
        "x:q=4:a", "x:d=3:a", "x:b=10:a", "x::3a", "x::a9", "x::a##", "x::a..", "x::", "a:b:c:d", "x:d=8,o=5,b=63:",
        "x:d=32,o=8,b=900:b#,c#.,1p,32h8.", "x:o=3,b=20:1c.,2d#3", "x:d=4,o=6,b=63:,,a,,", "x:d=4 ,o=6:16a,16a,16a",
        "x:d=16:a,8,c", "x:d=4,o=6,b=00063:a", "x:d=4,b=12345:a", "x:dd=4:a", "x:d=:a", "x:d4:a", "nonotes",
        "x::p,p", "x::c.6.", "x::c6.#", "x::#c", "x::00004c", "doorbell:d=8,o=6,b=180:e,c,4g",
    };
    std::string json = "[";
    {
      std::string many = "x::";
      for (size_t k = 0; k <= sound::kMaxMelodyNotes; k++)
        many += "p,";
      cases.push_back(many);                  // one pause too many
      many.resize(many.size() - 2);
      cases.push_back(many);                  // just enough
    }
    for (const std::string &c : cases) {
      std::vector<sound::Note> n;
      std::string error;
      const bool ok = sound::melody_notes(c, n, &error);
      json += std::string(json.size() > 1 ? ",\n" : "\n") + "{\"in\":\"" + c + "\",\"ok\":" + (ok ? "true" : "false");
      if (ok) {
        json += ",\"notes\":[";
        for (size_t k = 0; k < n.size(); k++)
          json += (k ? "," : "") + std::string("[") + std::to_string(n[k].hz) + "," + std::to_string(n[k].ms) + "]";
        json += "]";
      } else {
        std::string quoted;
        for (const char ch : error)
          quoted += ch == '"' ? std::string("\\\"") : std::string(1, ch);
        json += ",\"error\":\"" + quoted + "\"";
      }
      json += "}";
    }
    json += "\n]\n";
    std::ofstream("test/host/build/melody_cases.json") << json;
  }

  // melodies of one's own: the web interface's limit is the firmware's, and its names are those melody_name takes
  {
    std::smatch m;
    CHECK(std::regex_search(html, m, std::regex(R"(const MAX_MELODIES = (\d+);)")) &&
          std::stoul(m[1]) == sound::kMaxOwnMelodies);
    CHECK(sound::melody_name("doorbell") && sound::melody_name("a") && sound::melody_name("siren_2-b"));
    CHECK(!sound::melody_name("") && !sound::melody_name("Doorbell") && !sound::melody_name("door bell") &&
          !sound::melody_name("-x") && !sound::melody_name(std::string(25, 'a')));
    CHECK(!sound::melody_name("2") && !sound::melody_name("2nd-bell"));   // a letter first: no key JS would reorder
    CHECK(!sound::melody_name("none") && !sound::melody_name("beep"));
  }

  if (g_failures) {
    std::fprintf(stderr, "%d sound check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("sound: all checks passed\n");
  return 0;
}
