#include "Png.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <miniz.h>   // the ESP32's ROM: tinfl_decompress
#endif

namespace appdisplay {

#ifdef ESP_PLATFORM
namespace {

/* tinfl from the ROM, its output into a 32 KB ring: the window a deflate stream refers back into. */
class RomInflate : public Inflate {
 public:
  bool ready() const { return r_ && ring_; }
  RomInflate()
      : r_(static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_DEFAULT))),
        ring_(static_cast<uint8_t *>(heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_DEFAULT))) {
    if (r_)
      tinfl_init(r_);
  }
  ~RomInflate() override {
    heap_caps_free(r_);
    heap_caps_free(ring_);
  }
  int feed(const uint8_t *in, size_t n, const std::function<bool(const uint8_t *, size_t)> &out) override {
    for (;;) {
      size_t in_size = n, out_size = TINFL_LZ_DICT_SIZE - at_;
      const tinfl_status st = tinfl_decompress(r_, in, &in_size, ring_, ring_ + at_, &out_size,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
      in += in_size;
      n -= in_size;
      if (out_size > 0 && !out(ring_ + at_, out_size))
        return 1;
      at_ = (at_ + out_size) & (TINFL_LZ_DICT_SIZE - 1);
      if (st == TINFL_STATUS_DONE)
        return 1;
      if (st < 0)
        return -1;
      if (st != TINFL_STATUS_HAS_MORE_OUTPUT && n == 0)
        return 0;
    }
  }

 private:
  tinfl_decompressor *r_;
  uint8_t *ring_;
  size_t at_ = 0;
};

}  // namespace

std::unique_ptr<Inflate> Inflate::make() {
  auto in = std::make_unique<RomInflate>();
  return in->ready() ? std::move(in) : nullptr;
}
#endif

namespace {

uint32_t be32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

std::string decode_png(const Chunks &bytes, int size, const uint8_t under[3], PngPicture &out) {
  uint8_t head[13];
  if (bytes.read(0, head, 8) != 8 || std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) != 0)
    return "not a PNG";
  int w = 0, h = 0, depth = 0, type = -1, channels = 0;
  size_t row_bytes = 0, px_bytes = 1;
  int step = 1;
  uint8_t palette[256][3] = {}, alpha[256];
  std::memset(alpha, 255, sizeof(alpha));
  int colours = 0;
  int key[3] = {-1, -1, -1};   // tRNS of grey or RGB: the transparent colour, as stored
  std::vector<uint32_t> sums;
  std::vector<uint8_t> row, prior;
  size_t filled = 0;
  int y = 0;
  std::unique_ptr<Inflate> inflate;
  bool ended = false;

  // a row complete: unfiltered, its pixels added to their cells
  const auto take_row = [&] {
    uint8_t *line = row.data() + 1;
    const uint8_t *up = prior.data() + 1;
    const uint8_t f = row[0];
    for (size_t i = 0; i < row_bytes; i++) {
      const int a = i >= px_bytes ? line[i - px_bytes] : 0, b = up[i], c = i >= px_bytes ? up[i - px_bytes] : 0;
      switch (f) {
        case 1: line[i] = static_cast<uint8_t>(line[i] + a); break;
        case 2: line[i] = static_cast<uint8_t>(line[i] + b); break;
        case 3: line[i] = static_cast<uint8_t>(line[i] + (a + b) / 2); break;
        case 4: line[i] = static_cast<uint8_t>(line[i] + paeth(a, b, c)); break;
        default: break;
      }
    }
    const int gw = (w + step - 1) / step, cy = y / step;
    const int max = (1 << depth) - 1;
    for (int x = 0; x < w; x++) {
      // the samples as stored (raw: for the transparent colour) and as 8 bits
      int raw[4] = {0, 0, 0, 0};
      uint8_t v[4] = {0, 0, 0, 255};
      for (int ch = 0; ch < channels; ch++) {
        if (depth == 16) {
          const uint8_t *s = line + (static_cast<size_t>(x) * channels + ch) * 2;
          raw[ch] = s[0] << 8 | s[1];
          v[ch] = s[0];
        } else if (depth == 8) {
          raw[ch] = line[static_cast<size_t>(x) * channels + ch];
          v[ch] = static_cast<uint8_t>(raw[ch]);
        } else {   // 1, 2, 4 bits: grey or a palette index, one channel
          const size_t bit = static_cast<size_t>(x) * depth;
          raw[ch] = line[bit / 8] >> (8 - depth - bit % 8) & max;
          v[ch] = static_cast<uint8_t>(type == 3 ? raw[ch] : raw[ch] * 255 / max);
        }
      }
      uint8_t rgb[3], a = 255;
      switch (type) {
        case 0:   // grey
          rgb[0] = rgb[1] = rgb[2] = v[0];
          a = raw[0] == key[0] ? 0 : 255;
          break;
        case 2:   // RGB
          std::copy(v, v + 3, rgb);
          a = raw[0] == key[0] && raw[1] == key[1] && raw[2] == key[2] ? 0 : 255;
          break;
        case 3:   // a palette's
          if (raw[0] < colours) {
            std::copy(palette[raw[0]], palette[raw[0]] + 3, rgb);
            a = alpha[raw[0]];
          } else {
            rgb[0] = rgb[1] = rgb[2] = 0;
          }
          break;
        case 4:   // grey and alpha
          rgb[0] = rgb[1] = rgb[2] = v[0];
          a = v[1];
          break;
        default:  // RGB and alpha
          std::copy(v, v + 3, rgb);
          a = v[3];
      }
      uint32_t *cell = &sums[(static_cast<size_t>(cy) * gw + x / step) * 3];
      for (int c = 0; c < 3; c++)
        cell[c] += (rgb[c] * a + under[c] * (255 - a) + 127) / 255;
    }
    std::swap(row, prior);
    y++;
  };

  size_t at = 8;
  uint8_t buf[1024];
  while (!ended) {
    uint8_t chunk[8];
    if (bytes.read(at, chunk, 8) != 8)
      return "a broken PNG";
    const uint32_t len = be32(chunk);
    const std::string kind(reinterpret_cast<const char *>(chunk + 4), 4);
    if (len > bytes.size || at + 12 + len > bytes.size)
      return "a broken PNG";
    const size_t data = at + 8;
    if (kind == "IHDR") {
      if (len != 13 || bytes.read(data, head, 13) != 13)
        return "a broken PNG";
      w = static_cast<int>(be32(head));
      h = static_cast<int>(be32(head + 4));
      depth = head[8];
      type = head[9];
      if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return "a PNG of " + std::to_string(w) + " x " + std::to_string(h) + " px";
      if (head[12] != 0)
        return "an interlaced PNG, the panel decodes only plain ones";
      const bool ok = (type == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                      (type == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                      ((type == 2 || type == 4 || type == 6) && (depth == 8 || depth == 16));
      if (!ok || head[10] != 0 || head[11] != 0)
        return "a PNG of a kind the panel does not read";
      channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
      row_bytes = (static_cast<size_t>(w) * channels * depth + 7) / 8;
      px_bytes = std::max<size_t>(1, static_cast<size_t>(channels) * depth / 8);
      while (std::min(w, h) / (step + 1) >= size)
        step++;
      row.assign(row_bytes + 1, 0);
      prior.assign(row_bytes + 1, 0);
      sums.assign(static_cast<size_t>((w + step - 1) / step) * ((h + step - 1) / step) * 3, 0);
    } else if (kind == "PLTE") {
      colours = static_cast<int>(std::min<uint32_t>(len / 3, 256));
      bytes.read(data, &palette[0][0], static_cast<size_t>(colours) * 3);
    } else if (kind == "tRNS" && type >= 0) {
      uint8_t t[256] = {};
      const size_t n = bytes.read(data, t, std::min<uint32_t>(len, sizeof(t)));
      if (type == 3)
        std::copy(t, t + n, alpha);
      else if (type == 0 && n >= 2)
        key[0] = t[0] << 8 | t[1];
      else if (type == 2 && n >= 6)
        for (int c = 0; c < 3; c++)
          key[c] = t[2 * c] << 8 | t[2 * c + 1];
    } else if (kind == "IDAT") {
      if (type < 0)
        return "a broken PNG";
      if (!inflate && !(inflate = Inflate::make()))
        return "no memory to decode it";
      for (size_t done = 0; done < len && y < h;) {
        const size_t n = bytes.read(data + done, buf, std::min<size_t>(sizeof(buf), len - done));
        done += n;
        const int st = inflate->feed(buf, n, [&](const uint8_t *p, size_t m) {
          while (m > 0 && y < h) {
            const size_t take = std::min(m, row.size() - filled);
            std::memcpy(row.data() + filled, p, take);
            filled += take;
            p += take;
            m -= take;
            if (filled == row.size()) {
              take_row();
              filled = 0;
            }
          }
          return y < h;
        });
        if (st < 0)
          return "a broken PNG";
      }
    } else if (kind == "IEND") {
      ended = true;
    }
    at = data + len + 4;   // and its CRC
    if (y >= h && type >= 0)
      break;
  }
  if (type < 0 || y < h)
    return "a broken PNG";
  const int gw = (w + step - 1) / step, gh = (h + step - 1) / step;
  out.w = gw;
  out.h = gh;
  out.pixels.assign(static_cast<size_t>(gw) * gh * 3, 0);
  for (int cy = 0; cy < gh; cy++)
    for (int cx = 0; cx < gw; cx++) {
      const uint32_t n = static_cast<uint32_t>(std::min(step, w - cx * step) * std::min(step, h - cy * step));
      for (int c = 0; c < 3; c++) {
        const size_t i = (static_cast<size_t>(cy) * gw + cx) * 3 + c;
        out.pixels[i] = static_cast<uint8_t>((sums[i] + n / 2) / n);
      }
    }
  return "";
}

}  // namespace appdisplay
