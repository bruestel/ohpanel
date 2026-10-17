/*
   The panel's own temperature sensor (an NTC on GPIO38).

   Sampled in the main loop; the value is smoothed, corrected by the offset
   from the settings (the panel warms itself) and, with an item in the
   settings, reported to openHAB as that item's state.
*/

#pragma once

#include <cstdint>

namespace temperature {

/* Sets up the ADC. False if it is not available; celsius() then stays NAN. */
bool begin();

/* Call from the main loop; samples every two seconds, reports every minute
   (only when the value changed, at least every ten minutes). */
void tick(int64_t now_us);

/* Corrected by the offset; NAN until the first reading or without a sensor. */
double celsius();

/* As measured, without the offset -- for working out the offset. */
double raw_celsius();

}  // namespace temperature
