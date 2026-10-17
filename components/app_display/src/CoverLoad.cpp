/*
   Loading a cover picture on the device (Engine draws it, Cover.h): the
   picture fetched (or taken from a data: address), decoded by the ESP32's
   own JPEG decoder in ROM -- scaled down by it already, 1/2 to 1/8 --, cut
   to a square in its middle, averaged down to the size asked for and turned
   into rectangles. In a task of its own: the display task must not wait for
   the network. Only the latest request counts.
*/

#include "CoverLoad.h"

#include "appoh/OpenHab.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <rom/tjpgd.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace appdisplay {
namespace {

const char *const TAG = "cover";
constexpr size_t kMaxBytes = 400 * 1024;   // a cover as streaming services serve them: 640 px, up to ~150 KB
constexpr size_t kPool = 3100;             // the decoder's work area (TJpgDec: 3100 bytes at most)

struct Request {
  std::string source;
  int size = 0, scale = 1, radius = 0;
  Reveal reveal = Reveal::Dissolve;
};

std::mutex g_mx;
Request g_next;            // the request waiting, source empty: none
bool g_waiting = false;
SemaphoreHandle_t g_wake = nullptr;
CoverDone g_done;

/* The decoder's input (the bytes in memory) and output (the picture at its scale). */
struct Jpeg {
  const uint8_t *data;
  size_t size, at = 0;
  uint8_t *pixels = nullptr;   // w x h RGB
  int w = 0, h = 0;
};

UINT jpeg_in(JDEC *d, BYTE *buf, UINT n) {
  Jpeg &j = *static_cast<Jpeg *>(d->device);
  n = static_cast<UINT>(std::min<size_t>(n, j.size - j.at));
  if (buf)
    memcpy(buf, j.data + j.at, n);
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
   pixels on its shorter side; false for anything the ROM decoder cannot
   read (PNG, progressive JPEG). */
bool decode(const std::string &bytes, int size, Jpeg &j) {
  void *pool = heap_caps_malloc(kPool, MALLOC_CAP_DEFAULT);
  if (!pool)
    return false;
  JDEC dec;
  j.data = reinterpret_cast<const uint8_t *>(bytes.data());
  j.size = bytes.size();
  bool ok = jd_prepare(&dec, jpeg_in, pool, kPool, &j) == JDR_OK;
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
  return ok;
}

/* The square in the picture's middle, averaged down (or repeated up) to size x size. */
std::vector<uint8_t> square(const Jpeg &j, int size) {
  std::vector<uint8_t> out(static_cast<size_t>(size) * size * 3);
  const int side = std::min(j.w, j.h), ox = (j.w - side) / 2, oy = (j.h - side) / 2;
  for (int y = 0; y < size; y++) {
    const int y0 = oy + y * side / size, y1 = std::max(y0 + 1, oy + (y + 1) * side / size);
    for (int x = 0; x < size; x++) {
      const int x0 = ox + x * side / size, x1 = std::max(x0 + 1, ox + (x + 1) * side / size);
      uint32_t sum[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = j.pixels + (static_cast<size_t>(yy) * j.w + xx) * 3;
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

std::shared_ptr<Cover> load(const Request &r) {
  std::string bytes, error;
  if (r.source.rfind("data:", 0) == 0) {
    if (!data_uri_bytes(r.source, bytes)) {
      ESP_LOGW(TAG, "not a base64 data address");
      return nullptr;
    }
  } else if (!appoh::fetch(r.source, kMaxBytes, bytes, &error)) {
    ESP_LOGW(TAG, "%.80s: %s", r.source.c_str(), error.c_str());
    return nullptr;
  }
  Jpeg j;
  const bool ok = decode(bytes, r.size, j);
  std::string().swap(bytes);
  std::shared_ptr<Cover> cover;
  if (ok) {
    const std::vector<uint8_t> pixels = square(j, r.size);
    cover = std::make_shared<Cover>(cover_rects(pixels.data(), r.size, r.scale, r.radius, r.reveal));
    ESP_LOGI(TAG, "%dx%d px, %u rectangles", j.w, j.h, static_cast<unsigned>(cover->rects.size()));
  } else {
    ESP_LOGW(TAG, "%.80s: not a JPEG the decoder reads", r.source.c_str());
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

void cover_load(const std::string &source, int size, int scale, int radius, Reveal reveal) {
  if (!g_wake)
    return;
  {
    std::lock_guard<std::mutex> lock(g_mx);
    g_next = {source, size, scale, radius, reveal};
    g_waiting = true;
  }
  xSemaphoreGive(g_wake);
}

}  // namespace appdisplay
