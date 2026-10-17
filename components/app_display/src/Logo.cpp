#include "appdisplay/Logo.h"

namespace appdisplay {

namespace {

/* base64url without padding (the alphabet tools/station_logo.py writes) */
bool unbase64url(const std::string &in, std::vector<uint8_t> &out) {
  uint32_t bits = 0;
  int n = 0;
  for (const char ch : in) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '-') v = 62;
    else if (ch == '_') v = 63;
    else if (ch == '=') break;
    else return false;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    n += 6;
    if (n >= 8) {
      n -= 8;
      out.push_back(static_cast<uint8_t>(bits >> n));
    }
  }
  return true;
}

}  // namespace

bool decode_logo(const std::string &code, Logo &out) {
  std::vector<uint8_t> b;
  if (code.size() < 8 || !unbase64url(code, b) || b.size() < 4 || b[0] != 1)
    return false;
  const int w = b[1], h = b[2], colours = b[3];
  const size_t rects_at = 4 + 3 * static_cast<size_t>(colours);
  if (w == 0 || h == 0 || w > 64 || h > 64 || b.size() < rects_at || (b.size() - rects_at) % 5)
    return false;
  Logo logo;
  logo.w = w;
  logo.h = h;
  for (int i = 0; i < colours; i++) {
    const uint8_t *c = &b[4 + 3 * i];
    logo.palette.push_back({static_cast<uint16_t>(c[0] | c[1] << 8), c[2]});
  }
  for (size_t at = rects_at; at < b.size(); at += 5) {
    const Logo::Rect r{b[at], b[at + 1], b[at + 2], b[at + 3], b[at + 4]};
    if (r.colour >= colours || r.w == 0 || r.h == 0 || r.x + r.w > w || r.y + r.h > h)
      return false;
    logo.rects.push_back(r);
  }
  out = std::move(logo);
  return true;
}

uint16_t blend565(Logo::Colour c, uint16_t under) {
  if (c.alpha == 255)
    return c.rgb565;
  const auto mix = [a = c.alpha](int top, int bottom) { return (top * a + bottom * (255 - a) + 127) / 255; };
  const int r = mix(c.rgb565 >> 11, under >> 11);
  const int g = mix((c.rgb565 >> 5) & 0x3F, (under >> 5) & 0x3F);
  const int b = mix(c.rgb565 & 0x1F, under & 0x1F);
  return static_cast<uint16_t>(r << 11 | g << 5 | b);
}

}  // namespace appdisplay
