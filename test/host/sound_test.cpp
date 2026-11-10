/*
   Host test for the buzzer's tones: the names the schema takes, the ones the
   firmware plays (Sound.cpp) and the ones the web interface offers
   (www/index.html, TONES) are the same, and the notifications' defaults of the
   firmware (Format.h) are the web interface's (NOTE_TONES).
*/

#include "appdisplay/Format.h"
#include "appdisplay/Sound.h"
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
  for (const auto &n : appdisplay::tone_names()) {
    firmware.insert(n);
    CHECK(appdisplay::tone_notes(n) && !appdisplay::tone_notes(n)->empty());
  }
  CHECK(!appdisplay::tone_notes("none"));

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
    std::vector<appdisplay::Note> n;
    std::string error;
    CHECK(appdisplay::melody_notes("test:d=4,o=4,b=120:a,8p,c5,2e.5", n, &error));
    CHECK(n.size() == 7 && n[0].hz == 440 && n[0].ms == 480 && n[1].hz == 0 && n[1].ms == 20);   // a, its break
    CHECK(n.size() == 7 && n[2].hz == 0 && n[2].ms == 250);                                       // 8p: no break
    CHECK(n.size() == 7 && n[3].hz == 523 && n[3].ms == 480);                                     // c5
    CHECK(n.size() == 7 && n[5].hz == 659 && n[5].ms == 1480 && n[6].ms == 20);                   // 2e.5: 1500 ms
    // the dot before the octave too, sharps, h for b, upper case and blanks; the defaults d=4, o=6, b=63
    CHECK(appdisplay::melody_notes("X: d=8, b=240 : C#.5, H, P", n) && n.size() == 5 && n[0].hz == 554 &&
          n[0].ms == 187 - 18 && n[2].hz == 1976 && n[2].ms == 125 - 12 && n[4].hz == 0);
    CHECK(appdisplay::melody_notes("::a", n) && n.size() == 2 && n[0].hz == 1760 && n[0].ms + n[1].ms == 952);
    CHECK(appdisplay::melody_notes("d=4:a", n) && n.size() == 2);   // defaults without a name
    CHECK(appdisplay::melody_notes("name:a,b", n) && n.size() == 4);  // a name without defaults
    // what is none: a note, a default, a length, an octave, nothing at all, too many notes
    CHECK(!appdisplay::melody_notes("x:d=4:a,x", n, &error) && error.find("\"x\"") != std::string::npos);
    CHECK(!appdisplay::melody_notes("x:q=4:a", n, &error) && error.find("q=4") != std::string::npos);
    CHECK(!appdisplay::melody_notes("x:d=3:a", n));
    CHECK(!appdisplay::melody_notes("x:b=10:a", n));
    CHECK(!appdisplay::melody_notes("x::3a", n) && !appdisplay::melody_notes("x::a9", n) &&
          !appdisplay::melody_notes("x::a##", n) && !appdisplay::melody_notes("x::a..", n));
    CHECK(!appdisplay::melody_notes("x::", n, &error) && error == "no notes");
    CHECK(!appdisplay::melody_notes("a:b:c:d", n));
    CHECK(!appdisplay::melody_notes(std::string("x::a,") + '\0', n));
    std::string many = "x::";
    for (size_t k = 0; k <= appdisplay::kMaxMelodyNotes; k++)
      many += "a,";
    CHECK(!appdisplay::melody_notes(many, n, &error) && error.find("notes") != std::string::npos);
    // a tone's name or a melody
    CHECK(appdisplay::sound_notes("beep", n) && n.size() == 1);
    CHECK(appdisplay::sound_notes("x::a", n) && n.size() == 2);
    CHECK(!appdisplay::sound_notes("bang", n, &error) && error == "no such tone");
  }

  // the web interface's examples: const MELODIES = [['name', 'rtttl' + 'rtttl'], ...]; each plays
  {
    const size_t from = html.find("const MELODIES = ["), to = html.find("];", from);
    CHECK(from != std::string::npos && to != std::string::npos);
    const std::string list = from == std::string::npos ? "" : html.substr(from, to - from);
    const std::regex melody(R"(\['([^']+)', ('[^\]]+')\])");
    int count = 0;
    for (auto it = std::sregex_iterator(list.begin(), list.end(), melody); it != std::sregex_iterator(); ++it) {
      std::string rtttl;   // its pieces joined
      const std::string pieces = (*it)[2];
      const std::regex piece("'([^']*)'");
      for (auto p = std::sregex_iterator(pieces.begin(), pieces.end(), piece); p != std::sregex_iterator(); ++p)
        rtttl += (*p)[1];
      std::vector<appdisplay::Note> n;
      std::string error;
      if (!appdisplay::melody_notes(rtttl, n, &error)) {
        std::fprintf(stderr, "FAIL the example %s: %s\n", std::string((*it)[1]).c_str(), error.c_str());
        g_failures++;
      }
      count++;
    }
    CHECK(count >= 5);
  }

  if (g_failures) {
    std::fprintf(stderr, "%d sound check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("sound: all checks passed\n");
  return 0;
}
