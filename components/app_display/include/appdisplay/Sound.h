/*
   The tones of the buzzer (display.sound): a name for each, and the notes it
   plays; melodies in RTTTL. Free of ESP-IDF -- the engine picks a tone, the board's buzzer plays
   its notes (boards/nspanel/board/buzzer.h).
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace appdisplay {

struct Note {
  uint16_t hz;  // 0: a pause
  uint16_t ms;
  uint8_t level = 100;  // % of the volume: a note that fades out in steps sounds softer
};

/* The notes of a tone; null for "none" and a name that is none of them. */
const std::vector<Note> *tone_notes(const std::string &name);

/* Every tone's name ("none" not among them). */
std::vector<std::string> tone_names();

/* A melody in RTTTL, the ringtones' notation: "name:d=4,o=5,b=100:8e6,8d#6,e,p,2c.7" -- a name, the defaults
   (d: a note's length as a fraction of a whole one, 1 to 32; o: its octave, 3 to 8; b: beats, quarters, a minute),
   then the notes: [length]note[#][.][octave][.], the note c, d, e, f, g, a, b (or h), p a pause, a dot half as long
   again. The name and the defaults may be left out ("d=8:c,e,g", "::c,e,g"; default d=4, o=6, b=63). Its notes in
   `out`, each sounding to a short break before the next one, so that two of the same are heard as two; false for
   text that is no melody, and why in `error`. At most kMaxMelodyNotes notes. */
constexpr size_t kMaxMelodyNotes = 400;
bool melody_notes(const std::string &rtttl, std::vector<Note> &out, std::string *error = nullptr);

/* A tone's name or a melody: what plays it; false for neither (and why, for a melody, in `error`). */
bool sound_notes(const std::string &sound, std::vector<Note> &out, std::string *error = nullptr);

}  // namespace appdisplay
