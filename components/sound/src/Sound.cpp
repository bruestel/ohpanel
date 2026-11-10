/*
   The buzzer's tones. Its piezo is loudest at about 2.7 to 4 kHz; lower notes
   come out quieter and softer.
   The names are also in the schema (components/panel/src/Schema.cpp) and the
   web interface (www/index.html, TONES); test/host/sound_test.cpp checks they agree.
*/

#include "sound/Sound.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace sound {
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

namespace {

bool fail(std::string *error, const std::string &why) {
  if (error)
    *error = why;
  return false;
}

/* A number at `at` (digits only), moved past; -1 for none, or one of more than 4 digits. */
int number_at(const std::string &s, size_t &at) {
  const size_t from = at;
  int n = 0;
  while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])) && at - from < 4)
    n = n * 10 + (s[at++] - '0');
  return at == from || (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) ? -1 : n;
}

bool is_length(int d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32; }

/* A note's semitones above c; -1 for no note (h is b, as in German). */
int semitone_of(char c) {
  switch (c) {
    case 'c': return 0;
    case 'd': return 2;
    case 'e': return 4;
    case 'f': return 5;
    case 'g': return 7;
    case 'a': return 9;
    case 'b':
    case 'h': return 11;
    default: return -1;
  }
}

}  // namespace

bool melody_notes(const std::string &rtttl, std::vector<Note> &out, std::string *error) {
  out.clear();
  std::string text;   // without blanks: "8e6, 8d#6" as "8e6,8d#6"
  for (const char c : rtttl)
    if (!std::isspace(static_cast<unsigned char>(c)))
      text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // name:defaults:notes, defaults:notes or notes alone
  const size_t last = text.rfind(':');
  const std::string notes = last == std::string::npos ? text : text.substr(last + 1);
  std::string defaults;
  if (last != std::string::npos) {
    const size_t first = text.find(':');
    if (first == last) {
      defaults = text.substr(0, last);
      if (defaults.find('=') == std::string::npos)
        defaults.clear();   // "name:notes"
    } else if (text.find(':', first + 1) != last) {
      return fail(error, "more than two colons");
    } else {
      defaults = text.substr(first + 1, last - first - 1);
    }
  }
  int length = 4, octave = 6, bpm = 63;
  for (size_t at = 0; at < defaults.size();) {
    const size_t end = std::min(defaults.find(',', at), defaults.size());
    const std::string d = defaults.substr(at, end - at);
    at = end + 1;
    if (d.empty())
      continue;
    size_t i = 2;
    const int v = d.size() > 2 && d[1] == '=' ? number_at(d, i) : -1;
    if (v < 0 || i != d.size())
      return fail(error, "\"" + d + "\" is no default (d=4, o=6, b=63)");
    if (d[0] == 'd' && is_length(v))
      length = v;
    else if (d[0] == 'o' && v >= 3 && v <= 8)
      octave = v;
    else if (d[0] == 'b' && v >= 20 && v <= 900)
      bpm = v;
    else
      return fail(error, "\"" + d + "\": d is 1, 2, 4, 8, 16 or 32, o 3 to 8, b 20 to 900");
  }
  const int whole = 240000 / bpm;   // ms
  for (size_t at = 0; at < notes.size();) {
    const size_t end = std::min(notes.find(',', at), notes.size());
    const std::string n = notes.substr(at, end - at);
    at = end + 1;
    if (n.empty())
      continue;
    size_t i = 0;
    int d = length;
    if (std::isdigit(static_cast<unsigned char>(n[0])))
      d = number_at(n, i);
    const char c = i < n.size() ? n[i++] : '?';
    const bool pause = c == 'p';
    int semitone = semitone_of(c);   // from c
    bool ok = is_length(d) && (semitone >= 0 || pause);
    if (ok && !pause && i < n.size() && n[i] == '#') {
      semitone++;
      i++;
    }
    bool dotted = false;
    if (i < n.size() && n[i] == '.') {
      dotted = true;
      i++;
    }
    int o = octave;
    if (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) {
      o = n[i++] - '0';
      ok = ok && o >= 3 && o <= 8;
    }
    if (i < n.size() && n[i] == '.' && !dotted) {
      dotted = true;
      i++;
    }
    if (!ok || i != n.size())
      return fail(error, "\"" + n + "\" is no note (e.g. 8c#6.)");
    if (out.size() >= 2 * kMaxMelodyNotes)
      return fail(error, "more than " + std::to_string(kMaxMelodyNotes) + " notes");
    const int ms = whole / d * (dotted ? 3 : 2) / 2;
    if (pause) {
      out.push_back({0, static_cast<uint16_t>(ms)});
      continue;
    }
    // a4 is 440 Hz; a short break at its end, a tenth of it (at most 20 ms)
    const double hz = 440.0 * std::pow(2.0, (semitone - 9) / 12.0 + (o - 4));
    const int gap = std::min(ms / 10, 20);
    out.push_back({static_cast<uint16_t>(std::lround(hz)), static_cast<uint16_t>(ms - gap)});
    if (gap)
      out.push_back({0, static_cast<uint16_t>(gap)});
  }
  if (out.empty())
    return fail(error, "no notes");
  return true;
}

bool melody_name(const std::string &name, std::string *error) {
  if (name.empty() || name.size() > 24)
    return fail(error, "a melody's name has 1 to 24 characters");
  for (const char c : name)
    if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
          c == '_'))
      return fail(error, "a melody's name has only a-z, 0-9, - and _");
  if (name[0] == '-' || name[0] == '_')
    return fail(error, "a melody's name begins with a letter or digit");
  if (name == "none" || tone_notes(name))
    return fail(error, "\"" + name + "\" is a tone's name already");
  return true;
}

bool sound_notes(const std::string &sound, std::vector<Note> &out, std::string *error) {
  if (const std::vector<Note> *notes = tone_notes(sound)) {
    out = *notes;
    return true;
  }
  if (sound.find_first_of(":,") == std::string::npos)
    return fail(error, "no such tone");
  return melody_notes(sound, out, error);
}

}  // namespace sound
