/*
   The log for the web interface: the last lines ESP-IDF logs, kept in a ring
   buffer (in PSRAM) besides going to the serial port as before.
*/

#pragma once

#include <cstdint>
#include <string>

namespace logbuf {

/* Hooks into the log output; call as early as possible. */
void begin();

/* The text logged since position `since` (a running count of bytes), as far
   as the buffer still holds it; *next is the position to ask from next time.
   Lines older than the buffer are gone (the text then starts at the oldest
   whole line). */
std::string read(uint32_t since, uint32_t *next);

}  // namespace logbuf
