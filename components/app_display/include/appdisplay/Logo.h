#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace appdisplay {

/* A station logo as the station catalogue's metadata carries it (images=,
   made by tools/station_logo.py): filled rectangles in a palette whose
   colours may be translucent. Nothing to decode but the code's bytes; the
   panel lays the colours on what is under the logo and draws the rectangles. */
struct Logo {
  struct Colour {
    uint16_t rgb565;
    uint8_t alpha;  // 255: opaque
  };
  struct Rect {
    uint8_t x, y, w, h, colour;
  };
  int w = 0, h = 0;
  std::vector<Colour> palette;
  std::vector<Rect> rects;
};

/* The logo of a code; false (and `out` untouched) for one that is not. */
bool decode_logo(const std::string &code, Logo &out);

/* A colour of the logo laid on `under` (both RGB565). */
uint16_t blend565(Logo::Colour c, uint16_t under);

}  // namespace appdisplay
