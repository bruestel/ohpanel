/*
   The panel's buzzer: a passive piezo on GPIO21, driven by a square wave of
   the note's frequency (LEDC). A task of its own plays the notes, so a call
   only hands them over and returns; a tone that comes while another plays
   cuts it off.
*/

#pragma once

#include <cstdint>
#include <vector>

namespace buzzer {

struct Note {
  uint16_t hz;  // 0: a pause
  uint16_t ms;
  uint8_t level = 100;  // % of the volume
};

/* Sets up the pin and the task. False if either is not available; play() then does nothing. */
bool begin();

/* Plays the notes at `volume` (1 to 100; 0 plays nothing), each at its level of it. */
void play(const std::vector<Note> &notes, int volume);

/* Stops what plays. */
void stop();

}  // namespace buzzer
