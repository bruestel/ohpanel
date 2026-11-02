/*
   A PNG cover decoded as it is read (CoverLoad.cpp): the chunks walked in the
   bytes, the image data inflated piece by piece, each row unfiltered and added
   to a grid `step` x `step` pixels a cell -- the picture is never whole in
   memory, a 600 x 600 Sonos station logo takes a few KB. Grey, RGB, palette (1
   to 8 bits), with alpha or a transparent colour, 8 or 16 bits; not interlaced.
   Transparent parts lie on `under`, the art box's colour.
*/

#pragma once

#include "appdisplay/Cover.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace appdisplay {

/* A streaming zlib inflater: the ESP32's ROM (tinfl) on the device, zlib in the
   host tests. */
class Inflate {
 public:
  virtual ~Inflate() = default;
  /* `in` fed on; what comes out handed to `out` (false from it stops). 1: the
     stream is complete, 0: more input wanted, -1: broken. */
  virtual int feed(const uint8_t *in, size_t n, const std::function<bool(const uint8_t *, size_t)> &out) = 0;
  static std::unique_ptr<Inflate> make();   // null without memory
};

struct PngPicture {
  int w = 0, h = 0;              // the grid: the picture's size / step, rounded up
  std::vector<uint8_t> pixels;   // w x h RGB, each cell's average
};

/* The PNG in `bytes` decoded onto a grid of cells `step` x `step` pixels, the
   largest step that leaves its shorter side at least `size` cells; "" when it
   was, else why not. */
std::string decode_png(const Chunks &bytes, int size, const uint8_t under[3], PngPicture &out);

}  // namespace appdisplay
