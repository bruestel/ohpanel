#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace appdisplay {

/* What is playing as a picture (the media page's cover, item "art"), ready
   to draw: the display cannot take a picture at runtime, so the panel draws
   it as filled rectangles -- a few thousand for a photo, from the middle
   outwards. Made from the picture scaled down to `size` x `size` pixels,
   each drawn `scale` px large, inside a box with rounded corners. */
struct Cover {
  struct Rect {
    uint16_t x, y, w, h;   // display pixels, from the box's top left corner
    uint16_t colour;       // RGB565
  };
  int side = 0;   // the box: side x side display pixels
  std::vector<Rect> rects;
};

/* How a cover appears (display.cover_reveal): dissolving in, in a random
   order; from the middle outwards; blurred first (8 x 8, then 4 x 4 pixels
   averaged) and then sharp; wiped on from the top. */
enum class Reveal { Dissolve, Center, Sharpen, Wipe };
Reveal reveal_of(const std::string &name);   // unknown or empty: Dissolve

/* The rectangles of a picture of `size` x `size` RGB pixels (3 bytes each,
   row by row): each pixel `scale` px large, colours in 5 bits per channel
   (neighbours alike become one rectangle), corners rounded with `radius`
   display px (what lies outside is left out: the box's picture shows
   there). In the order `reveal` draws them. */
Cover cover_rects(const uint8_t *rgb, int size, int scale, int radius, Reveal reveal = Reveal::Center);

/* A cover's address in an item: an http(s) address, a path on the openHAB
   server ("/...") or the picture itself ("data:image/...;base64,..."). */
bool cover_source(const std::string &text);

/* The bytes of a "data:...;base64,..." address; false for anything else. */
bool data_uri_bytes(const std::string &uri, std::string &out);

}  // namespace appdisplay
