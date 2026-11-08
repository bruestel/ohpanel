/*
   The tones of the buzzer (display.sound): a name for each, and the notes it
   plays. Free of ESP-IDF -- the engine picks a tone, the board's buzzer plays
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

}  // namespace appdisplay
