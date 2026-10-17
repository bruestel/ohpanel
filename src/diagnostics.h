/*
   Knowing what went wrong on a device you cannot reach.

   Once the panel sits in its wall box there is no console and no cable. If
   it reboots, the only evidence is whatever it kept: why the last restart
   happened, whether a crash was recorded, and how close the tasks are running to
   their stack limits.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace diagnostics {

/* Registers this task with the watchdog and records why the device last
   restarted. Call early, before anything can hang. */
void begin();

/* Feed the watchdog. Call from the main loop; a loop that stops running is
   exactly the condition worth rebooting for. */
void feed();

/* Reset reason of the current boot, in words. */
const char *boot_reason();
bool crashed_last_boot();

/* Rendered for the HTTP layer. Tasks, stack headroom, heap, reset reason. */
std::string health_json();

/* Now and then (the main loop's minute): a warning in the log for a task whose stack came within 1 KB of its
   end (the least it ever had left), once -- and again for each further 256 bytes. ESP-IDF's own tasks with
   their small fixed stacks (idle, ipc, the event loop) are left out. */
void check_stacks();

/* The stored core dump, for offline analysis with esp-coredump. Returns the
   number of bytes written, or 0 if there is none. */
size_t coredump_size();
bool read_coredump(size_t offset, void *buf, size_t len);
bool erase_coredump();

}  // namespace diagnostics
