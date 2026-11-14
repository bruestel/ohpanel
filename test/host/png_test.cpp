// Host tests of the PNG cover decoder (components/app_display/src/Png.cpp): PNGs made here, of every colour type
// and filter, decoded with zlib in place of the ESP32's ROM inflater.

#include "Png.h"

#include <zlib.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace appdisplay;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                       \
    }                                                                   \
  } while (0)

namespace appdisplay {

class ZlibInflate : public Inflate {
 public:
  ZlibInflate() { inflateInit(&z_); }
  ~ZlibInflate() override { inflateEnd(&z_); }
  int feed(const uint8_t *in, size_t n, const std::function<bool(const uint8_t *, size_t)> &out) override {
    z_.next_in = const_cast<Bytef *>(in);
    z_.avail_in = static_cast<uInt>(n);
    uint8_t buf[300];   // small: rows arrive split
    for (;;) {
      z_.next_out = buf;
      z_.avail_out = sizeof(buf);
      const int st = inflate(&z_, Z_NO_FLUSH);
      const size_t got = sizeof(buf) - z_.avail_out;
      if (got > 0 && !out(buf, got))
        return 1;
      if (st == Z_STREAM_END)
        return 1;
      if (st != Z_OK && st != Z_BUF_ERROR)
        return -1;
      if (z_.avail_in == 0 && got < sizeof(buf))
        return 0;
    }
  }

 private:
  z_stream z_{};
};

std::unique_ptr<Inflate> Inflate::make() { return std::make_unique<ZlibInflate>(); }

}  // namespace appdisplay

namespace {

void put32(std::string &s, uint32_t v) {
  for (int i = 3; i >= 0; i--)
    s.push_back(static_cast<char>(v >> (8 * i) & 0xFF));
}

void chunk(std::string &png, const char *type, const std::string &data) {
  put32(png, static_cast<uint32_t>(data.size()));
  std::string body = type + data;
  png += body;
  put32(png, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(body.data()), body.size())));
}

/* A PNG of raw rows (`rows`: h rows of row_bytes each, unfiltered), each row filtered with filter (y % 5), the image
   data split into IDAT chunks of `split` bytes. */
std::string make_png(int w, int h, int depth, int type, const std::vector<std::vector<uint8_t>> &rows, int bpp,
                     const std::string &extra = "", size_t split = 97) {
  std::string raw;
  std::vector<uint8_t> prior(rows[0].size(), 0);
  for (int y = 0; y < h; y++) {
    const std::vector<uint8_t> &line = rows[y];
    const int f = y % 5;
    raw.push_back(static_cast<char>(f));
    for (size_t i = 0; i < line.size(); i++) {
      const int a = i >= static_cast<size_t>(bpp) ? line[i - bpp] : 0, b = prior[i];
      const int c = i >= static_cast<size_t>(bpp) ? prior[i - bpp] : 0;
      int pred = 0;
      if (f == 1) pred = a;
      if (f == 2) pred = b;
      if (f == 3) pred = (a + b) / 2;
      if (f == 4) {
        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      raw.push_back(static_cast<char>((line[i] - pred) & 0xFF));
    }
    prior = line;
  }
  uLongf len = compressBound(raw.size());
  std::string z(len, '\0');
  compress(reinterpret_cast<Bytef *>(z.data()), &len, reinterpret_cast<const Bytef *>(raw.data()), raw.size());
  z.resize(len);
  std::string png = "\x89PNG\r\n\x1a\n", ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr += std::string{static_cast<char>(depth), static_cast<char>(type), 0, 0, 0};
  chunk(png, "IHDR", ihdr);
  png += extra;
  for (size_t at = 0; at < z.size(); at += split)
    chunk(png, "IDAT", z.substr(at, split));
  chunk(png, "IEND", "");
  return png;
}

Chunks chunks_of(const std::string &s) {
  Chunks c;
  for (size_t at = 0; at < s.size(); at += 1000)
    c.append(s.data() + at, std::min<size_t>(1000, s.size() - at));
  return c;
}

const uint8_t kUnder[3] = {44, 44, 44};

/* The pixel at (x, y) of a picture decoded at step 1. */
std::array<int, 3> at(const PngPicture &p, int x, int y) {
  const uint8_t *q = &p.pixels[(static_cast<size_t>(y) * p.w + x) * 3];
  return {q[0], q[1], q[2]};
}

}  // namespace

int main() {
  const int W = 13, H = 11;   // odd sizes: rows not a multiple of anything
  const auto colour = [](int x, int y, int c) { return static_cast<uint8_t>((x * 37 + y * 11 + c * 80) & 0xFF); };

  {  // RGB, 8 bits, every filter: back as it was
    std::vector<std::vector<uint8_t>> rows(H);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        for (int c = 0; c < 3; c++)
          rows[y].push_back(colour(x, y, c));
    PngPicture p;
    CHECK(decode_png(chunks_of(make_png(W, H, 8, 2, rows, 3)), 11, kUnder, p).empty());
    CHECK(p.w == W && p.h == H);
    bool same = true;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        same &= at(p, x, y) == std::array<int, 3>{colour(x, y, 0), colour(x, y, 1), colour(x, y, 2)};
    CHECK(same);
  }
  {  // RGBA: transparent on the box's grey, opaque as it is, half between
    std::vector<std::vector<uint8_t>> rows(H);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const uint8_t a = x == 0 ? 0 : x == 1 ? 255 : 128;
        rows[y].insert(rows[y].end(), {200, 100, 0, a});
      }
    PngPicture p;
    CHECK(decode_png(chunks_of(make_png(W, H, 8, 6, rows, 4)), 11, kUnder, p).empty());
    CHECK((at(p, 0, 5) == std::array<int, 3>{44, 44, 44}) && (at(p, 1, 5) == std::array<int, 3>{200, 100, 0}));
    CHECK((at(p, 2, 5) == std::array<int, 3>{122, 72, 22}));
  }
  {  // 16 bits RGB: the high bytes
    std::vector<std::vector<uint8_t>> rows(H);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        for (int c = 0; c < 3; c++)
          rows[y].insert(rows[y].end(), {colour(x, y, c), 0x55});
    PngPicture p;
    CHECK(decode_png(chunks_of(make_png(W, H, 16, 2, rows, 6)), 11, kUnder, p).empty());
    CHECK((at(p, 4, 7) == std::array<int, 3>{colour(4, 7, 0), colour(4, 7, 1), colour(4, 7, 2)}));
  }
  {  // a palette of 4 bits with a transparent entry, as Sonos station logos may come
    std::vector<std::vector<uint8_t>> rows(H);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x += 2) {
        const int a = (x + y) % 3, b = x + 1 < W ? (x + 1 + y) % 3 : 0;
        rows[y].push_back(static_cast<uint8_t>(a << 4 | b));
      }
    std::string extra;
    chunk(extra, "PLTE", std::string("\xff\x00\x00\x00\xff\x00\x00\x00\xff", 9));
    chunk(extra, "tRNS", std::string("\xff\xff\x00", 3));   // the third colour transparent
    PngPicture p;
    CHECK(decode_png(chunks_of(make_png(W, H, 4, 3, rows, 1, extra)), 11, kUnder, p).empty());
    CHECK((at(p, 0, 0) == std::array<int, 3>{255, 0, 0}) && (at(p, 1, 0) == std::array<int, 3>{0, 255, 0}));
    CHECK((at(p, 2, 0) == std::array<int, 3>{44, 44, 44}) && (at(p, 0, 1) == std::array<int, 3>{0, 255, 0}));
  }
  {  // grey of 1 bit, grey and alpha
    std::vector<std::vector<uint8_t>> rows(H), ga(H);
    for (int y = 0; y < H; y++) {
      rows[y].assign((W + 7) / 8, 0);
      for (int x = 0; x < W; x++) {
        if ((x + y) % 2)
          rows[y][x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
        ga[y].insert(ga[y].end(), {static_cast<uint8_t>(x * 20), 255});
      }
    }
    PngPicture p, q;
    CHECK(decode_png(chunks_of(make_png(W, H, 1, 0, rows, 1)), 11, kUnder, p).empty());
    CHECK((at(p, 0, 0) == std::array<int, 3>{0, 0, 0}) && (at(p, 1, 0) == std::array<int, 3>{255, 255, 255}));
    CHECK(decode_png(chunks_of(make_png(W, H, 8, 4, ga, 2)), 11, kUnder, q).empty());
    CHECK((at(q, 3, 2) == std::array<int, 3>{60, 60, 60}));
  }
  {  // averaged onto cells: 600 x 600 into steps of 7 (85 cells) for a cover of 80
    const int n = 600;
    std::vector<std::vector<uint8_t>> rows(n);
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        rows[y].insert(rows[y].end(), {static_cast<uint8_t>(x < 300 ? 0 : 255), 128, 0});
    PngPicture p;
    CHECK(decode_png(chunks_of(make_png(n, n, 8, 2, rows, 3, "", 4096)), 80, kUnder, p).empty());
    CHECK(p.w == 86 && p.h == 86);   // 600 / 7, rounded up
    CHECK(at(p, 0, 0)[0] == 0 && at(p, 85, 85)[0] == 255 && at(p, 42, 10)[0] == 36);   // 6 of its 7 columns left of 300
  }
  {  // what it does not read, and broken ones
    std::vector<std::vector<uint8_t>> rows(H, std::vector<uint8_t>(W * 3, 9));
    std::string interlaced = make_png(W, H, 8, 2, rows, 3);
    interlaced[8 + 8 + 12] = 1;   // IHDR's interlace byte
    PngPicture p;
    CHECK(decode_png(chunks_of(interlaced), 11, kUnder, p).rfind("an interlaced PNG", 0) == 0);
    const std::string whole = make_png(W, H, 8, 2, rows, 3);
    CHECK(decode_png(chunks_of(whole.substr(0, whole.size() / 2)), 11, kUnder, p) == "a broken PNG");
    CHECK(decode_png(chunks_of("\xff\xd8\xff\xe0 a JPEG"), 11, kUnder, p) == "not a PNG");
  }
  /* A data: text read as it comes (DataUriReader: openHAB's answer for an Image item's state) gives the bytes
     data_uri_bytes gives for it whole, however it is cut; a text that is none is refused. */
  {
    std::string bytes;
    for (int i = 0; i < 5000; i++)
      bytes += static_cast<char>(i * 31 + 7);
    static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text = "data:image/jpeg;base64,";
    for (size_t i = 0; i < bytes.size(); i += 3) {
      uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
      if (i + 1 < bytes.size()) v |= static_cast<uint8_t>(bytes[i + 1]) << 8;
      if (i + 2 < bytes.size()) v |= static_cast<uint8_t>(bytes[i + 2]);
      text += abc[v >> 18 & 63];
      text += abc[v >> 12 & 63];
      text += i + 1 < bytes.size() ? abc[v >> 6 & 63] : '=';
      text += i + 2 < bytes.size() ? abc[v & 63] : '=';
    }
    Chunks whole;
    CHECK(data_uri_bytes(text, whole) && whole.size == bytes.size());
    for (size_t piece : {1, 5, 1000}) {
      Chunks out;
      DataUriReader r;
      bool ok = true;
      for (size_t at = 0; at < text.size(); at += piece)
        ok = ok && r.feed(text.data() + at, std::min(piece, text.size() - at), out);
      CHECK(ok && r.finish(out) && out.size == bytes.size());
      std::string got(out.size, '\0');
      out.read(0, reinterpret_cast<uint8_t *>(got.data()), out.size);
      CHECK(got == bytes);
    }
    Chunks out;
    DataUriReader bad;
    CHECK(!bad.feed("<html>,no", 9, out));
    DataUriReader plain;
    CHECK(!plain.feed("data:text/plain,hello", 21, out));
  }
  if (failures) {
    std::printf("%d png check(s) failed\n", failures);
    return 1;
  }
  std::printf("png: all checks passed\n");
  return 0;
}
