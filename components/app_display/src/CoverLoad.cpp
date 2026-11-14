/*
   Loading a cover picture on the device (Engine draws it, Cover.h): the
   picture fetched (or taken from a data: address) in parts of 16 KB (Chunks: no large block in one piece), anything
   but a JPEG or a PNG left at its first bytes, a JPEG decoded by the ESP32's
   own JPEG decoder in ROM -- scaled down by it already, 1/2 to 1/8 --, a PNG row by row (Png.h), cut
   to a square in its middle, averaged down to the size asked for and turned
   into rectangles. In a task of its own: the display task must not wait for
   the network. Only the latest request counts.
*/

#include "CoverLoad.h"
#include "Png.h"

#include "appoh/OpenHab.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <rom/tjpgd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace appdisplay {
namespace {

const char *const TAG = "cover";
constexpr size_t kMaxBytes = 400 * 1024;   // a cover as streaming services serve them: 640 px, up to ~150 KB
constexpr size_t kPool = 3100;             // the decoder's work area (TJpgDec: 3100 bytes at most)

struct Request {
  std::string source;
  int size = 0, scale = 1, radius = 0;
  Reveal reveal = Reveal::Dissolve;
  int fit_w = 0, fit_h = 0;   // > 0: the whole picture within fit_w x fit_h pixels (its shape kept), not a square
};

std::mutex g_mx;
Request g_next;            // the request waiting, source empty: none
bool g_waiting = false;
SemaphoreHandle_t g_wake = nullptr;
CoverDone g_done;

/* The decoder's input (the bytes in memory, in parts) and output (the picture at its scale). */
struct Jpeg {
  const Chunks *data;
  size_t at = 0;
  uint8_t *pixels = nullptr;   // w x h RGB
  int w = 0, h = 0;
};

UINT jpeg_in(JDEC *d, BYTE *buf, UINT n) {
  Jpeg &j = *static_cast<Jpeg *>(d->device);
  n = static_cast<UINT>(std::min<size_t>(n, j.data->size - j.at));
  if (buf)
    j.data->read(j.at, buf, n);
  j.at += n;
  return n;
}

UINT jpeg_out(JDEC *d, void *bitmap, JRECT *r) {
  Jpeg &j = *static_cast<Jpeg *>(d->device);
  const int bw = r->right - r->left + 1;
  const auto *src = static_cast<const uint8_t *>(bitmap);
  for (int y = r->top; y <= r->bottom && y < j.h; y++) {
    const int w = std::min(bw, j.w - r->left);
    if (w > 0)
      memcpy(j.pixels + (static_cast<size_t>(y) * j.w + r->left) * 3, src + (y - r->top) * bw * 3, w * 3);
  }
  return 1;
}

/* The JPEG decoded at the smallest of its scales still at least `size`
   pixels on its shorter side; "" when it was, else why not (a progressive
   JPEG, which the ROM decoder cannot read). */
std::string decode(const Chunks &bytes, int size, Jpeg &j) {
  void *pool = heap_caps_malloc(kPool, MALLOC_CAP_DEFAULT);
  if (!pool)
    return "no memory to decode it";
  JDEC dec;
  j.data = &bytes;
  const JRESULT prepared = jd_prepare(&dec, jpeg_in, pool, kPool, &j);
  bool ok = prepared == JDR_OK;
  if (ok) {
    int scale = 0;
    while (scale < 3 && static_cast<int>(std::min(dec.width, dec.height) >> (scale + 1)) >= size)
      scale++;
    j.w = static_cast<int>((dec.width + (1u << scale) - 1) >> scale);
    j.h = static_cast<int>((dec.height + (1u << scale) - 1) >> scale);
    j.pixels = static_cast<uint8_t *>(heap_caps_malloc(static_cast<size_t>(j.w) * j.h * 3,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!j.pixels)
      j.pixels = static_cast<uint8_t *>(heap_caps_malloc(static_cast<size_t>(j.w) * j.h * 3, MALLOC_CAP_8BIT));
    ok = j.pixels && jd_decomp(&dec, jpeg_out, static_cast<BYTE>(scale)) == JDR_OK;
  }
  heap_caps_free(pool);
  if (ok)
    return "";
  if (prepared == JDR_FMT3)
    return "a progressive JPEG, the panel decodes only baseline ones";
  if (prepared == JDR_OK)
    return j.pixels ? "a broken JPEG" : "no memory to decode it";
  return "a JPEG the decoder does not read";
}

/* The square in the picture's middle, averaged down (or repeated up) to size x size. */
std::vector<uint8_t> square(const uint8_t *pixels, int w, int h, int size) {
  std::vector<uint8_t> out(static_cast<size_t>(size) * size * 3);
  const int side = std::min(w, h), ox = (w - side) / 2, oy = (h - side) / 2;
  for (int y = 0; y < size; y++) {
    const int y0 = oy + y * side / size, y1 = std::max(y0 + 1, oy + (y + 1) * side / size);
    for (int x = 0; x < size; x++) {
      const int x0 = ox + x * side / size, x1 = std::max(x0 + 1, ox + (x + 1) * side / size);
      uint32_t sum[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = pixels + (static_cast<size_t>(yy) * w + xx) * 3;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          n++;
        }
      for (int c = 0; c < 3; c++)
        out[(static_cast<size_t>(y) * size + x) * 3 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
    }
  }
  return out;
}

/* The whole picture averaged down (or repeated up) to w x h. */
std::vector<uint8_t> resize(const uint8_t *pixels, int iw, int ih, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; y++) {
    const int y0 = y * ih / h, y1 = std::max(y0 + 1, (y + 1) * ih / h);
    for (int x = 0; x < w; x++) {
      const int x0 = x * iw / w, x1 = std::max(x0 + 1, (x + 1) * iw / w);
      uint32_t sum[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = pixels + (static_cast<size_t>(yy) * iw + xx) * 3;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          n++;
        }
      for (int c = 0; c < 3; c++)
        out[(static_cast<size_t>(y) * w + x) * 3 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
    }
  }
  return out;
}

std::shared_ptr<Cover> load(Request r) {
  // "#..." after the address is the engine's (a picture fetched again: another key), no server's business
  if (r.source.rfind("data:", 0) != 0)
    r.source.erase(std::min(r.source.find('#'), r.source.size()));
  // the decoder's least size: the shorter side of the box the picture fits in (no more of it is needed)
  if (r.fit_w > 0 && r.fit_h > 0)
    r.size = std::min(r.fit_w, r.fit_h);
  Chunks bytes;
  std::string error, what;
  if (r.source.rfind("data:", 0) == 0) {
    if (!data_uri_bytes(r.source, bytes)) {
      ESP_LOGW(TAG, "not a base64 data address");
      return nullptr;
    }
  } else {
    /* The picture, or a text with it (openHAB's answer for an Image item's state: "data:image/...;base64,...", the
       address the stream gave for a picture too large to send along, appoh::rest_state_path): decoded as it comes,
       the text never held. Its base64 is a third longer than the picture. */
    DataUriReader text;
    bool is_text = false, started = false;
    const bool ok = appoh::fetch(r.source, kMaxBytes * 4 / 3 + 1024, [&](const char *data, size_t n) {
      if (!started) {
        started = true;
        is_text = n >= 5 && std::memcmp(data, "data:", 5) == 0;
        // its first bytes say what it is: anything but a JPEG or a PNG goes no further
        if (!is_text && n >= 4 && !(what = not_jpeg(reinterpret_cast<const uint8_t *>(data), n)).empty() &&
            what != "a PNG")
          return false;
      }
      if (is_text) {
        if (!text.feed(data, n, bytes)) {
          what = "a text that is no picture";
          return false;
        }
      } else {
        bytes.append(data, n);
      }
      if (bytes.size > kMaxBytes) {
        what = "larger than " + std::to_string(kMaxBytes / 1024) + " KB";
        return false;
      }
      return true;
    }, &error);
    if (ok && is_text && !text.finish(bytes)) {
      ESP_LOGW(TAG, "%.80s: a text that is no picture", r.source.c_str());
      return nullptr;
    }
    if (!ok) {
      ESP_LOGW(TAG, "%.80s: %s", r.source.c_str(),
               what.empty() ? error.c_str()
               : what.rfind("a ", 0) == 0 && what != "a text that is no picture" ? (what + ", the panel decodes JPEG and PNG").c_str()
                                                                                   : what.c_str());
      return nullptr;
    }
  }
  uint8_t head[12] = {};
  const std::string kind = not_jpeg(head, bytes.read(0, head, sizeof(head)));
  if (!kind.empty() && kind != "a PNG") {
    ESP_LOGW(TAG, "%.80s: %s, the panel decodes JPEG and PNG", r.source.c_str(), kind.c_str());
    return nullptr;
  }
  // the picture at a scale still at least `size` on its shorter side: a JPEG by the ROM's decoder, a PNG row by row
  Jpeg j;
  PngPicture png;
  std::string why;
  if (kind.empty()) {
    why = decode(bytes, r.size, j);
  } else {
    static constexpr uint8_t kArtBox[3] = {44, 44, 44};   // what shows through: the art box (compose.py "art")
    why = decode_png(bytes, r.size, kArtBox, png);
  }
  const size_t fetched = bytes.size;
  bytes = Chunks{};
  std::shared_ptr<Cover> cover;
  if (why.empty()) {
    const int w = kind.empty() ? j.w : png.w, h = kind.empty() ? j.h : png.h;
    const uint8_t *decoded = kind.empty() ? j.pixels : png.pixels.data();
    if (r.fit_w > 0 && r.fit_h > 0) {   // the whole picture, as large as fits, its shape kept
      const double f = std::min(static_cast<double>(r.fit_w) / w, static_cast<double>(r.fit_h) / h);
      const int pw = std::clamp(static_cast<int>(std::lround(w * f)), 1, r.fit_w),
                ph = std::clamp(static_cast<int>(std::lround(h * f)), 1, r.fit_h);
      const std::vector<uint8_t> pixels = resize(decoded, w, h, pw, ph);
      cover = std::make_shared<Cover>(picture_rects(pixels.data(), pw, ph, r.scale, r.radius, r.reveal));
    } else {
      const std::vector<uint8_t> pixels = square(decoded, w, h, r.size);
      cover = std::make_shared<Cover>(cover_rects(pixels.data(), r.size, r.scale, r.radius, r.reveal));
    }
    ESP_LOGI(TAG, "%dx%d px%s, %u rectangles, from %u bytes of %.48s", w, h, kind.empty() ? "" : " (PNG)",
             static_cast<unsigned>(cover->rects.size()), static_cast<unsigned>(fetched), r.source.c_str());
  } else {
    ESP_LOGW(TAG, "%.80s: %s", r.source.c_str(), why.c_str());
  }
  heap_caps_free(j.pixels);
  return cover;
}

void task(void *) {
  for (;;) {
    xSemaphoreTake(g_wake, portMAX_DELAY);
    Request r;
    {
      std::lock_guard<std::mutex> lock(g_mx);
      if (!g_waiting)
        continue;
      r = g_next;
      g_waiting = false;
    }
    std::shared_ptr<Cover> cover = load(r);
    bool newer;
    {
      std::lock_guard<std::mutex> lock(g_mx);
      newer = g_waiting;
    }
    if (!newer && g_done)   // another one asked for meanwhile: this one is out of date
      g_done(r.source, cover);
  }
}

}  // namespace

void cover_begin(CoverDone done) {
  g_done = std::move(done);
  g_wake = xSemaphoreCreateBinary();
  if (!g_wake || xTaskCreate(task, "cover", 8192, nullptr, 3, nullptr) != pdPASS)
    ESP_LOGE(TAG, "no cover task");
}

void cover_load(const std::string &source, int size, int scale, int radius, Reveal reveal, int fit_w, int fit_h) {
  if (!g_wake)
    return;
  {
    std::lock_guard<std::mutex> lock(g_mx);
    g_next = {source, size, scale, radius, reveal, fit_w, fit_h};
    g_waiting = true;
  }
  xSemaphoreGive(g_wake);
}

}  // namespace appdisplay
