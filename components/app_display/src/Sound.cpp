/*
   The buzzer's tones. Its piezo is loudest at about 2.7 to 4 kHz; lower notes
   come out quieter and softer.
   The names are also in the schema (components/panel/src/Schema.cpp) and the
   web interface (www/index.html, TONES); test/host/sound_test.cpp checks they agree.
*/

#include "appdisplay/Sound.h"

#include <utility>

namespace appdisplay {
namespace {

const std::vector<std::pair<std::string, std::vector<Note>>> &tones() {
  static const std::vector<std::pair<std::string, std::vector<Note>>> t = {
      {"click", {{1100, 4}, {1100, 5, 45}, {1100, 4, 20}}},   // struck, then fading: soft
      {"tick", {{2700, 12}}},
      {"beep", {{2700, 120}}},
      {"double", {{2700, 70}, {0, 70}, {2700, 70}}},
      {"triple", {{2700, 60}, {0, 60}, {2700, 60}, {0, 60}, {2700, 60}}},
      {"rising", {{1568, 90}, {2093, 90}, {2637, 160}}},     // G6 C7 E7
      {"falling", {{2637, 90}, {2093, 90}, {1568, 160}}},
      {"ding-dong", {{1760, 220}, {0, 30}, {1397, 380}}},    // A6 F6
      {"alarm", {{3200, 120}, {2400, 120}, {3200, 120}, {2400, 120}, {3200, 120}, {2400, 120},
                 {3200, 120}, {2400, 120}}},
  };
  return t;
}

}  // namespace

const std::vector<Note> *tone_notes(const std::string &name) {
  for (const auto &[n, notes] : tones())
    if (n == name)
      return &notes;
  return nullptr;
}

std::vector<std::string> tone_names() {
  std::vector<std::string> names;
  for (const auto &t : tones())
    names.push_back(t.first);
  return names;
}

}  // namespace appdisplay
