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

  if (g_failures) {
    std::fprintf(stderr, "%d sound check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("sound: all checks passed\n");
  return 0;
}
