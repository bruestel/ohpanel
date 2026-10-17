/*
   The NTC on the panel's board, from the voltage at the ADC to degrees.

   The NTC sits between the ADC pin and ground, a fixed resistor between 3.3 V
   and the pin (see board.h), so the pin reads the divider
   V = 3.3 V * R_ntc / (R_ntc + R_fixed). The resistance follows from that,
   the temperature from the B-parameter equation
   1/T = 1/T25 + ln(R_ntc / R25) / B.

   Pure arithmetic, so the host tests check it.
*/

#pragma once

#include <cmath>

namespace ntc {

constexpr double kSupplyMv = 3300.0;
constexpr double kFixedOhm = 11200.0;
constexpr double kR25Ohm = 10000.0;
constexpr double kB = 3950.0;

/* Degrees Celsius for a pin voltage in millivolts; NAN outside what the
   divider can produce (a missing or shorted sensor). */
inline double celsius(double mv) {
  if (!(mv > 1.0 && mv < kSupplyMv - 1.0))
    return NAN;
  const double r = kFixedOhm * mv / (kSupplyMv - mv);
  const double inv_t = 1.0 / 298.15 + std::log(r / kR25Ohm) / kB;
  return 1.0 / inv_t - 273.15;
}

}  // namespace ntc
