#include "appdisplay/Cover.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace appdisplay {

namespace {

/* A pixel in 5 bits per channel, as RGB565 (green's sixth bit repeats its
   top one: white stays white). */
uint16_t quantize(const uint8_t *p) {
  const unsigned r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
  return static_cast<uint16_t>(r << 11 | (g << 1 | g >> 4) << 5 | b);
}

}  // namespace

Reveal reveal_of(const std::string &name) {
  if (name == "center") return Reveal::Center;
  if (name == "sharpen") return Reveal::Sharpen;
  if (name == "wipe") return Reveal::Wipe;
  return Reveal::Dissolve;
}

namespace {

/* The picture with every block of block x block pixels in its average colour. */
std::vector<uint8_t> blocks(const uint8_t *rgb, int size, int block) {
  std::vector<uint8_t> out(static_cast<size_t>(size) * size * 3);
  for (int by = 0; by < size; by += block)
    for (int bx = 0; bx < size; bx += block) {
      const int ey = std::min(size, by + block), ex = std::min(size, bx + block);
      uint32_t sum[3] = {0, 0, 0}, n = 0;
      for (int y = by; y < ey; y++)
        for (int x = bx; x < ex; x++, n++)
          for (int c = 0; c < 3; c++)
            sum[c] += rgb[(static_cast<size_t>(y) * size + x) * 3 + c];
      for (int y = by; y < ey; y++)
        for (int x = bx; x < ex; x++)
          for (int c = 0; c < 3; c++)
            out[(static_cast<size_t>(y) * size + x) * 3 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
    }
  return out;
}

/* The rectangles of one picture, from the middle outwards. */
std::vector<Cover::Rect> rects_of(const uint8_t *rgb, int size, int scale, int radius) {
  Cover out;
  const int side = size * scale;

  // runs of a colour along each row, a run below one of the same place and colour joining it
  struct Run {
    int x, y, w, h;
    uint16_t colour;
  };
  /* The runs of the row above and of this one, by x: two vectors, not a map: a map's nodes are 32 bytes, kept
     in internal RAM (only larger blocks go to PSRAM), and an 80 px cover's thousands of them took ~80 KB of it. */
  std::vector<Run> runs;
  std::vector<size_t> above, here;
  for (int y = 0; y < size; y++) {
    const uint8_t *row = rgb + static_cast<size_t>(y) * size * 3;
    size_t a = 0;
    here.clear();
    for (int x = 0; x < size;) {
      const uint16_t c = quantize(row + x * 3);
      int end = x + 1;
      while (end < size && quantize(row + end * 3) == c)
        end++;
      while (a < above.size() && runs[above[a]].x < x)
        a++;
      if (a < above.size() && runs[above[a]].x == x && runs[above[a]].w == end - x && runs[above[a]].colour == c) {
        runs[above[a]].h++;
        here.push_back(above[a]);
      } else {
        here.push_back(runs.size());
        runs.push_back({x, y, end - x, 1, c});
      }
      x = end;
    }
    above.swap(here);
  }

  // the corners: how far a display row near the top or bottom starts inside (a quarter circle)
  std::vector<int> inset(radius);
  for (int i = 0; i < radius; i++) {
    const double dy = radius - i - 0.5;
    inset[i] = radius - static_cast<int>(std::lround(std::sqrt(radius * radius - dy * dy)));
  }
  const auto inset_of = [&](int y) {
    if (y < radius) return inset[y];
    if (y >= side - radius) return inset[side - 1 - y];
    return 0;
  };
  const auto add = [&out](int x, int y, int w, int h, uint16_t c) {
    if (w > 0 && h > 0)
      out.rects.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(w),
                           static_cast<uint16_t>(h), c});
  };
  for (const Run &r : runs) {
    const int x0 = r.x * scale, x1 = (r.x + r.w) * scale, y0 = r.y * scale, y1 = (r.y + r.h) * scale;
    // clear of the corners' columns: whole
    if (x0 >= radius && x1 <= side - radius) {
      add(x0, y0, x1 - x0, y1 - y0, r.colour);
      continue;
    }
    // the rows of the corners cut to the circle, rows alike together; the rest whole
    for (int y = y0; y < y1;) {
      const int in = inset_of(y);
      int end = y + 1;
      while (end < y1 && inset_of(end) == in)
        end++;
      const int a = std::max(x0, in), b = std::min(x1, side - in);
      add(a, y, b - a, end - y, r.colour);
      y = end;
    }
  }

  // from the middle outwards: the picture grows from its centre instead of being wiped on from the top
  const auto distance = [side](const Cover::Rect &r) {
    const int dx = 2 * r.x + r.w - side, dy = 2 * r.y + r.h - side;   // twice the offset of its centre
    return dx * dx + dy * dy;
  };
  std::stable_sort(out.rects.begin(), out.rects.end(),
                   [&](const Cover::Rect &a, const Cover::Rect &b) { return distance(a) < distance(b); });
  return out.rects;
}

}  // namespace

Cover cover_rects(const uint8_t *rgb, int size, int scale, int radius, Reveal reveal) {
  Cover out;
  if (!rgb || size <= 0 || scale <= 0)
    return out;
  out.side = size * scale;
  radius = std::clamp(radius, 0, out.side / 2);
  if (reveal == Reveal::Sharpen)   // coarse blocks first, each pass over the one before
    for (const int block : {8, 4}) {
      const std::vector<uint8_t> coarse = blocks(rgb, size, block);
      const std::vector<Cover::Rect> pass = rects_of(coarse.data(), size, scale, radius);
      out.rects.insert(out.rects.end(), pass.begin(), pass.end());
    }
  std::vector<Cover::Rect> full = rects_of(rgb, size, scale, radius);
  if (reveal == Reveal::Wipe) {   // row by row from the top, each row from the left
    std::stable_sort(full.begin(), full.end(), [](const Cover::Rect &a, const Cover::Rect &b) {
      return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
  } else if (reveal == Reveal::Dissolve) {   // in a random order (the same for the same picture)
    uint32_t seed = 2463534242u;
    for (size_t i = full.size(); i > 1; i--) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      std::swap(full[i - 1], full[seed % i]);
    }
  }
  out.rects.insert(out.rects.end(), full.begin(), full.end());
  return out;
}

bool cover_source(const std::string &text) {
  return text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0 ||
         (text.size() > 1 && text[0] == '/' && text[1] != '/') || text.rfind("data:image/", 0) == 0;
}

void Chunks::append(const char *data, size_t n) {
  while (n > 0) {
    if (parts.empty() || parts.back().size() == kPart) {
      parts.emplace_back();
      parts.back().reserve(kPart);
    }
    const size_t take = std::min(n, kPart - parts.back().size());
    parts.back().append(data, take);
    data += take;
    n -= take;
    size += take;
  }
}

size_t Chunks::read(size_t at, uint8_t *out, size_t n) const {
  size_t done = 0;
  while (done < n && at < size) {
    const std::string &part = parts[at / kPart];
    const size_t off = at % kPart, take = std::min(n - done, part.size() - off);
    std::memcpy(out + done, part.data() + off, take);
    done += take;
    at += take;
  }
  return done;
}

std::string not_jpeg(const uint8_t *head, size_t n) {
  const auto starts = [&](const char *magic, size_t len) { return n >= len && std::memcmp(head, magic, len) == 0; };
  if (starts("\xff\xd8\xff", 3))
    return "";
  if (starts("\x89PNG", 4))
    return "a PNG";
  if (starts("GIF8", 4))
    return "a GIF";
  if (starts("RIFF", 4) && n >= 12 && std::memcmp(head + 8, "WEBP", 4) == 0)
    return "a WebP";
  if (starts("<", 1))
    return "a web page";
  return "not a picture it knows";
}

bool DataUriReader::feed(const char *data, size_t n, Chunks &out) {
  for (size_t i = 0; i < n; i++) {
    const char c = data[i];
    if (bad_ || done_)
      return !bad_;
    if (!in_body_) {   // "data:<type>;base64," first
      if (c != ',') {
        if (head_.size() >= 256) {
          bad_ = true;
          return false;
        }
        head_ += c;
        continue;
      }
      if (head_.rfind("data:", 0) != 0 || head_.size() < 12 || head_.compare(head_.size() - 7, 7, ";base64") != 0) {
        bad_ = true;
        return false;
      }
      in_body_ = true;
      continue;
    }
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') {
      done_ = true;
      continue;
    } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else {
      bad_ = true;
      return false;
    }
    acc_ = acc_ << 6 | static_cast<uint32_t>(v);
    bits_ += 6;
    if (bits_ >= 8) {
      bits_ -= 8;
      run_[in_run_++] = static_cast<char>(acc_ >> bits_ & 0xFF);
      if (in_run_ == sizeof(run_)) {
        out.append(run_, in_run_);
        in_run_ = 0;
      }
    }
  }
  return true;
}

bool DataUriReader::finish(Chunks &out) {
  out.append(run_, in_run_);
  in_run_ = 0;
  return in_body_ && !bad_ && out.size > 0;
}

bool data_uri_bytes(const std::string &uri, Chunks &out) {
  out = Chunks{};
  DataUriReader r;
  return r.feed(uri.data(), uri.size(), out) && r.finish(out);
}

}  // namespace appdisplay
