/*
   Loading a cover picture on the device (Engine draws it, Cover.h): the
   picture fetched (or taken from a data: address) in parts of 16 KB (Chunks: no large block in one piece), anything
   but a JPEG or a PNG left at its first bytes, a JPEG decoded by the ESP32's
   own JPEG decoder in ROM -- scaled down by it already, 1/2 to 1/8 --, a progressive one (which that cannot read: a
   Squeezebox server's covers) by lowzag, scaled down as well, its memory in PSRAM, a PNG row by row (Png.h) -- each
   as it downloads, the file never held (Stream; a data: address's picture is), cut
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
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <lowzag.h>
#include <rom/tjpgd.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace appdisplay {
namespace {

const char *const TAG = "cover";
constexpr size_t kMaxBytes = 400 * 1024;   // a picture held: a data: address's, an Image item's text
constexpr size_t kMaxStreamBytes = 8 * 1024 * 1024;   // a JPEG's or a PNG's file, decoded as it comes, never held
constexpr const char *kKeyMark = "#ohpanel:";   // a key's mark after the address (Engine draw_image)
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
std::string g_loading;     // the request being loaded (its source as asked for)
SemaphoreHandle_t g_wake = nullptr;
CoverDone g_done;

/* The decoder's input (the bytes in memory, in parts) and output (the picture at its scale). */
struct Jpeg {
  ByteSource *data = nullptr;
  size_t at = 0;
  uint8_t *pixels = nullptr;   // w x h RGB
  int w = 0, h = 0;
};

UINT jpeg_in(JDEC *d, BYTE *buf, UINT n) {
  Jpeg &j = *static_cast<Jpeg *>(d->device);
  n = static_cast<UINT>(j.data->read(j.at, buf, n));   // buf null: skipped
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

/* lowzag's input, output and memory (decode_progressive). */
int progressive_in(void *ctx, uint8_t *buf, size_t n) {
  Jpeg &j = *static_cast<Jpeg *>(ctx);
  const size_t got = j.data->read(j.at, buf, n);
  j.at += got;
  return static_cast<int>(got);
}

void progressive_row(void *ctx, int y, int w, const uint8_t *rgb) {
  Jpeg &j = *static_cast<Jpeg *>(ctx);
  if (y >= 0 && y < j.h)
    memcpy(j.pixels + static_cast<size_t>(y) * j.w * 3, rgb, static_cast<size_t>(std::min(w, j.w)) * 3);
}

void *progressive_alloc(void *, size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // the internal RAM only for small blocks: a large one there would starve WiFi and TLS
  return p || n > 16 * 1024 ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

void progressive_free(void *, void *p) { heap_caps_free(p); }

/* What a progressive JPEG leaves of the PSRAM free besides its decoded picture: for what goes on meanwhile. */
constexpr size_t kProgressiveReserve = 96 * 1024;
constexpr size_t kStreamBuffer = 16 * 1024;
constexpr int kStreamWaitMs = 30000;   // no byte for that long: given up
constexpr size_t kHeadBytes = 64 * 1024;   // a JPEG's frame header within its first that many bytes (after EXIF, ICC)

/* lowzag's settings for a progressive JPEG whose first bytes (its frame header among them) are `head`: the smallest
   scale (1/1 .. 1/8) still at least `size` pixels on each side, its working memory from PSRAM -- in several blocks (a
   component's coefficients, their bitmap, its strip), all of what is free there but the decoded picture and
   kProgressiveReserve: over that less colour resolution, a smaller scale, or not at all (some 2000 x 1500 take
   890 KB) --, the picture's memory taken (j.pixels, j.w, j.h). "" when it can be decoded, else why not. The input
   is the caller's (cfg.read). */
std::string progressive_plan(const uint8_t *head, size_t n, int size, lowzag_cfg_t &cfg, lowzag_estimate_t &est,
                             Jpeg &j, size_t extra = 0) {
  cfg = LOWZAG_CFG_INIT;
  cfg.row = progressive_row;
  cfg.row_ctx = &j;
  cfg.alloc = progressive_alloc;
  cfg.free = progressive_free;
  cfg.target_w = cfg.target_h = size;
  // the picture's size first (the scale and so its size do not depend on the limit, at 1/8 at least), then the
  // limit: what is free, less the picture and the reserve
  est = LOWZAG_ESTIMATE_INIT;
  cfg.mem_limit = 0;
  size_t picture = 0;
  if (lowzag_estimate(&cfg, head, n, &est) == LOWZAG_OK)
    picture = static_cast<size_t>(est.out_w) * est.out_h * 3;
  // (`extra`: what is taken besides meanwhile -- a stream's buffer)
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), keep = picture + kProgressiveReserve + extra;
  cfg.mem_limit = free > keep + 64 * 1024 ? free - keep : 64 * 1024;
  est = LOWZAG_ESTIMATE_INIT;
  if (const lowzag_err_t e = lowzag_estimate(&cfg, head, n, &est); e != LOWZAG_OK) {
    if (e == LOWZAG_ERR_NOMEM) {   // what it would take, for the log
      lowzag_cfg_t any = cfg;
      any.mem_limit = 0;
      lowzag_estimate_t need = LOWZAG_ESTIMATE_INIT;
      if (lowzag_estimate(&any, head, n, &need) == LOWZAG_OK)
        return "a progressive JPEG too large: " + std::to_string(need.src_w) + "x" + std::to_string(need.src_h) +
               " takes " + std::to_string(need.total_bytes / 1024) + " KB, " +
               std::to_string(cfg.mem_limit / 1024) + " KB are free for it";
    }
    return std::string("a progressive JPEG it could not read: ") + lowzag_strerror(e);
  }
  j.w = est.out_w;
  j.h = est.out_h;
  j.pixels = static_cast<uint8_t *>(progressive_alloc(nullptr, static_cast<size_t>(j.w) * j.h * 3));
  return j.pixels ? "" : "no memory to decode it";
}

void progressive_log(const lowzag_estimate_t &est, const lowzag_info_t &info, const Jpeg &j, bool streamed) {
  ESP_LOGI(TAG, "progressive JPEG %dx%d, decoded at 1/%d to %dx%d, %u KB at most%s%s", est.src_w, est.src_h,
           info.scale, j.w, j.h, static_cast<unsigned>(info.peak_alloc / 1024),
           info.chroma_steps > 0 ? ", its colours at less resolution" : "", streamed ? ", as it came" : "");
}

/* A progressive JPEG whose bytes are all here (a data: address, an Image item's text): decoded by lowzag. */
std::string decode_progressive(const Chunks &bytes, int size, Jpeg &j) {
  // the frame's header, for the scale and the memory it takes: within the file's first part (16 KB) as a rule --
  // a header behind a larger EXIF or ICC block is read from a copy of the first 64 KB
  std::vector<uint8_t> head(std::min<size_t>(bytes.size, kHeadBytes));
  bytes.read(0, head.data(), head.size());
  lowzag_cfg_t cfg;
  lowzag_estimate_t est;
  if (std::string why = progressive_plan(head.data(), head.size(), size, cfg, est, j); !why.empty())
    return why;
  head = {};
  ChunkSource in(bytes);
  j.data = &in;
  j.at = 0;
  cfg.read = progressive_in;
  cfg.read_ctx = &j;
  lowzag_info_t info = LOWZAG_INFO_INIT;
  if (const lowzag_err_t e = lowzag_decode(&cfg, &info); e != LOWZAG_OK)
    return std::string("a progressive JPEG it could not decode: ") + lowzag_strerror(e);
  progressive_log(est, info, j, false);
  return "";
}

/* A picture decoded as it downloads -- each decoder reads its file once, front to back --, the file never held whole:
   the bytes go through a stream buffer to a task of its own that decodes them (`job`). The download waits while the
   buffer is full; it stops when the decoder does. */
struct Stream : ByteSource {
  /* Its memory in PSRAM, made here: ESP-IDF 6.0.1's vStreamBufferDeleteWithCaps() deletes a stream buffer as a
     queue (vSemaphoreDelete), reads past its end whether it was allocated statically and now and then frees it
     itself -- then again: the heap's "block already marked as free". */
  StaticStreamBuffer_t *meta = nullptr;
  uint8_t *storage = nullptr;
  StreamBufferHandle_t buf = nullptr;
  SemaphoreHandle_t finished = nullptr;
  std::atomic<bool> done{false};   // all sent (or the download broke off): what is left in the buffer is the end
  std::atomic<bool> over{false};   // the decoder no longer reads
  std::atomic<bool> cancel{false};   // a newer picture asked for: the decoder to stop
  std::function<std::string()> job;   // the decoding: "" when it went, else why not
  std::string why;
  size_t pos = 0;                  // bytes taken from the buffer
  bool started = false;
  // a progressive JPEG's
  lowzag_cfg_t cfg;
  lowzag_estimate_t est;
  lowzag_info_t info = LOWZAG_INFO_INIT;

  ~Stream() {
    if (started) {   // never left running: told the input is over, waited for
      done = true;
      xSemaphoreTake(finished, portMAX_DELAY);
    }
    if (buf)
      vStreamBufferDelete(buf);   // static: frees nothing
    heap_caps_free(meta);
    heap_caps_free(storage);
    if (finished)
      vSemaphoreDelete(finished);
  }

  /* Up to n bytes as they come (at least one while there are more): 0 at the end, -1 when none came for long. */
  int some(uint8_t *out, size_t n) {
    for (int waited = 0; waited < kStreamWaitMs && !cancel; waited += 100) {
      const bool end = done;   // read before: what was sent before it is in the buffer
      if (const size_t got = xStreamBufferReceive(buf, out, n, pdMS_TO_TICKS(100)); got > 0) {
        pos += got;
        return static_cast<int>(got);
      }
      if (end && xStreamBufferIsEmpty(buf))
        return 0;
    }
    return -1;
  }

  size_t read(size_t at, uint8_t *out, size_t n) override {
    if (at < pos)   // gone already
      return 0;
    uint8_t scrap[256];
    while (pos < at) {   // skipped
      const int got = some(scrap, std::min(sizeof(scrap), at - pos));
      if (got <= 0)
        return 0;
    }
    size_t total = 0;
    while (total < n) {
      const int got = out ? some(out + total, n - total) : some(scrap, std::min(sizeof(scrap), n - total));
      if (got <= 0)
        break;
      total += static_cast<size_t>(got);
    }
    return total;
  }
};

int stream_in(void *ctx, uint8_t *buf, size_t n) { return static_cast<Stream *>(ctx)->some(buf, n); }

void stream_task(void *arg) {
  Stream &s = *static_cast<Stream *>(arg);
  s.why = s.job();
  s.over = true;
  xSemaphoreGive(s.finished);
  vTaskDelete(nullptr);
}

constexpr const char *kNoTask = "no memory for a decoder of its own";

/* The decoder's task started on `job`; "" when it runs, else why not (kNoTask: it is decoded once all is here). */
std::string stream_start(Stream &s, std::function<std::string()> job) {
  s.job = std::move(job);
  s.meta = static_cast<StaticStreamBuffer_t *>(heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s.storage = static_cast<uint8_t *>(heap_caps_malloc(kStreamBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s.meta && s.storage)
    s.buf = xStreamBufferCreateStatic(kStreamBuffer, 1, s.storage, s.meta);
  s.finished = xSemaphoreCreateBinary();
  if (!s.buf || !s.finished || xTaskCreate(stream_task, "cover_decode", 8192, &s, 3, nullptr) != pdPASS)
    return kNoTask;
  s.started = true;
  return "";
}

/* A progressive JPEG's decoder started on what has come so far (`head`: its frame header among it). */
std::string stream_progressive(Stream &s, const uint8_t *head, size_t n, int size, Jpeg &j) {
  if (std::string why = progressive_plan(head, n, size, s.cfg, s.est, j, kStreamBuffer + n); !why.empty())
    return why;
  s.cfg.read = stream_in;
  s.cfg.read_ctx = &s;
  s.cfg.cancel = [](void *ctx) { return static_cast<Stream *>(ctx)->cancel ? 1 : 0; };
  s.cfg.cancel_ctx = &s;
  std::string why = stream_start(s, [&s, &j] {
    if (const lowzag_err_t e = lowzag_decode(&s.cfg, &s.info); e != LOWZAG_OK)
      return std::string("a progressive JPEG it could not decode: ") + lowzag_strerror(e);
    progressive_log(s.est, s.info, j, true);
    return std::string();
  });
  if (!why.empty()) {   // the picture's memory back: taken again when it is decoded once all is here
    heap_caps_free(j.pixels);
    j.pixels = nullptr;
  }
  return why;
}

/* Another picture asked for meanwhile: the one loading is out of date (the same one asked again is not). */
bool superseded() {
  std::lock_guard<std::mutex> lock(g_mx);
  return g_waiting && g_next.source != g_loading;
}

/* Bytes for the decoder; false once it reads no more (done, or given up). */
bool stream_send(Stream &s, const char *data, size_t n) {
  while (n > 0) {
    if (s.over)
      return false;
    const size_t sent = xStreamBufferSend(s.buf, data, n, pdMS_TO_TICKS(100));
    data += sent;
    n -= sent;
  }
  return true;
}

/* The JPEG decoded at the smallest of its scales still at least `size`
   pixels on its shorter side; "" when it was, else why not. A progressive one, which the ROM decoder cannot read,
   goes to lowzag. */
std::string decode_baseline(ByteSource &in, int size, Jpeg &j, JRESULT &prepared);

std::string decode(const Chunks &bytes, int size, Jpeg &j) {
  ChunkSource in(bytes);
  JRESULT prepared = JDR_OK;
  std::string why = decode_baseline(in, size, j, prepared);
  if (prepared == JDR_FMT3)
    return decode_progressive(bytes, size, j);
  return why;
}

/* A JPEG by the ROM's decoder, from `in` as it comes; `prepared` says whether it could read its header (JDR_FMT3:
   one it does not read, a progressive one). */
std::string decode_baseline(ByteSource &in, int size, Jpeg &j, JRESULT &prepared) {
  void *pool = heap_caps_malloc(kPool, MALLOC_CAP_DEFAULT);
  if (!pool)
    return "no memory to decode it";
  JDEC dec;
  j.data = &in;
  j.at = 0;
  prepared = jd_prepare(&dec, jpeg_in, pool, kPool, &j);
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
  // "#ohpanel:..." after the address is the engine's (whose picture it is, one fetched again), no server's business --
  // a data: address's too (its base64 has no '#'); a "#" of the address's own is a fragment, never sent either
  r.source.erase(std::min(r.source.find(kKeyMark), r.source.size()));
  if (r.source.rfind("data:", 0) != 0)
    r.source.erase(std::min(r.source.find('#'), r.source.size()));
  // the decoder's least size: the shorter side of the box the picture fits in (no more of it is needed)
  if (r.fit_w > 0 && r.fit_h > 0)
    r.size = std::min(r.fit_w, r.fit_h);
  Chunks bytes;
  std::string error, what;
  static constexpr uint8_t kArtBox[3] = {44, 44, 44};   // what shows through a PNG: the art box (compose.py "art")
  Jpeg j;          // before the stream: its decoder writes into them till the stream is gone
  PngPicture png;
  Stream stream;   // a JPEG or a PNG decoded as it comes
  bool streaming = false, decoder_stopped = false, cancelled = false, broke = false;
  size_t streamed = 0;
  if (r.source.rfind("data:", 0) == 0) {
    if (!data_uri_bytes(r.source, bytes)) {
      ESP_LOGW(TAG, "not a base64 data address");
      return nullptr;
    }
  } else {
    /* The picture, or a text with it (openHAB's answer for an Image item's state: "data:image/...;base64,...", the
       address the stream gave for a picture too large to send along, appoh::rest_state_path): decoded as it comes,
       the text never held. Its base64 is a third longer than the picture. A JPEG or a PNG itself goes to its decoder
       as it comes (a JPEG once its frame header shows which), not held at all. */
    DataUriReader text;
    bool is_text = false, started = false;
    /* which decoder, as it comes: 0 not known yet (a JPEG's frame header not here yet), 'p' lowzag's (progressive),
       'b' the ROM's, 'n' a PNG's; 'm' held and decoded once all is here (no task to decode it as it comes) */
    char frame = 0;
    size_t probe_at = 2048;   // a JPEG's first bytes looked at again from that many on (twice as many each time)
    std::string head;   // its first bytes, however the answer comes in parts: what it is
    const auto decide = [&]() {
      started = true;
      is_text = head.size() >= 5 && head.compare(0, 5, "data:") == 0;
      // anything but a JPEG or a PNG goes no further
      return is_text || (what = not_jpeg(reinterpret_cast<const uint8_t *>(head.data()), head.size())).empty() ||
             what == "a PNG";
    };
    const auto take = [&](const char *data, size_t n) {
      if (superseded()) {
        cancelled = stream.cancel = true;
        return false;
      }
      if (streaming) {
        streamed += n;
        decoder_stopped = !stream_send(stream, data, n);
        return !decoder_stopped;
      }
      if (is_text) {
        if (!text.feed(data, n, bytes)) {
          what = "a text that is no picture";
          return false;
        }
      } else {
        bytes.append(data, n);
        if (frame == 0 && what == "a PNG") {
          frame = 'n';
        } else if (frame == 0 && bytes.size >= probe_at) {   // a JPEG: which frame (lowzag_probe), once it is here
          std::vector<uint8_t> first(std::min(bytes.size, kHeadBytes));
          bytes.read(0, first.data(), first.size());
          const lowzag_err_t e = lowzag_probe(first.data(), first.size(), nullptr, nullptr);
          frame = e == LOWZAG_OK ? 'p' : e == LOWZAG_ERR_TRUNCATED && first.size() < kHeadBytes ? 0 : 'b';
          probe_at = bytes.size * 2;
        }
        if (frame == 'p' || frame == 'b' || frame == 'n') {   // the decoder started on what is here
          std::vector<uint8_t> first(bytes.size);
          bytes.read(0, first.data(), first.size());
          const int size = r.size;
          std::string why = frame == 'p' ? stream_progressive(stream, first.data(), first.size(), size, j)
                            : frame == 'n'
                                ? stream_start(stream, [&stream, &png, size] { return decode_png(stream, size, kArtBox, png); })
                                : stream_start(stream, [&stream, &j, size] {
                                    JRESULT prepared = JDR_OK;
                                    std::string why = decode_baseline(stream, size, j, prepared);
                                    return prepared == JDR_FMT3 ? std::string("a JPEG the decoder does not read") : why;
                                  });
          if (why == kNoTask) {
            frame = 'm';   // held: decoded once all is here, as before
          } else if (!why.empty()) {
            what = why;
            return false;
          } else {
            bytes = Chunks{};
            streaming = true;
            streamed = first.size();
            decoder_stopped = !stream_send(stream, reinterpret_cast<const char *>(first.data()), first.size());
            return !decoder_stopped;
          }
        }
      }
      if (bytes.size > kMaxBytes) {
        what = "larger than " + std::to_string(kMaxBytes / 1024) + " KB";
        return false;
      }
      return true;
    };
    bool ok = appoh::fetch(r.source, kMaxStreamBytes, [&](const char *data, size_t n) {
      if (started)
        return take(data, n);
      head.append(data, n);
      if (head.size() < 12)   // not enough yet to tell (a PNG's, a WebP's mark: up to 12 bytes)
        return true;
      return decide() && take(head.data(), head.size());
    }, &error);
    if (ok && !started)   // an answer shorter than that
      ok = decide() && take(head.data(), head.size());
    if (streaming) {   // the input over: the decoder's end waited for (stopped when a newer picture is asked for)
      stream.done = true;
      while (xSemaphoreTake(stream.finished, pdMS_TO_TICKS(200)) != pdTRUE)
        if (!stream.cancel && superseded())
          cancelled = stream.cancel = true;
      stream.started = false;
      broke = !ok && !decoder_stopped && !cancelled;   // the download broke off: that, not what the decoder made of it
      ok = true;
    }
    if (cancelled) {
      ESP_LOGI(TAG, "%.80s: left for a newer picture", r.source.c_str());
      heap_caps_free(j.pixels);
      return nullptr;
    }
    if (ok && is_text && !text.finish(bytes)) {
      ESP_LOGW(TAG, "%.80s: a text that is no picture", r.source.c_str());
      return nullptr;
    }
    if (!ok) {
      ESP_LOGW(TAG, "%.80s: %s", r.source.c_str(),
               what.empty() ? error.c_str()
               : what.rfind("a ", 0) == 0 && what != "a text that is no picture" &&
                       what.rfind("a progressive", 0) != 0
                   ? (what + ", the panel decodes JPEG and PNG").c_str()
                   : what.c_str());
      heap_caps_free(j.pixels);
      return nullptr;
    }
  }
  // the picture at a scale still at least `size` on its shorter side: a JPEG by the ROM's decoder (a progressive one
  // by lowzag), a PNG row by row -- decoded already, as it came, but a data: address's
  std::string why, kind = what == "a PNG" ? what : "";
  if (streaming) {
    why = stream.why.empty() || !broke ? stream.why : error;
  } else {
    uint8_t head[12] = {};
    kind = not_jpeg(head, bytes.read(0, head, sizeof(head)));
    if (!kind.empty() && kind != "a PNG") {
      ESP_LOGW(TAG, "%.80s: %s, the panel decodes JPEG and PNG", r.source.c_str(), kind.c_str());
      return nullptr;
    }
    if (kind.empty())
      why = decode(bytes, r.size, j);
    else
      why = decode_png(bytes, r.size, kArtBox, png);
  }
  const size_t fetched = streaming ? streamed : bytes.size;
  bytes = Chunks{};
  std::shared_ptr<Cover> cover;
  const auto free_decoded = [&j, &png] {
    heap_caps_free(j.pixels);
    j.pixels = nullptr;
    png.pixels = {};
  };
  if (why.empty()) {
    const int w = kind.empty() ? j.w : png.w, h = kind.empty() ? j.h : png.h;
    const uint8_t *decoded = kind.empty() ? j.pixels : png.pixels.data();
    if (r.fit_w > 0 && r.fit_h > 0) {   // the whole picture, as large as fits, its shape kept
      const double f = std::min(static_cast<double>(r.fit_w) / w, static_cast<double>(r.fit_h) / h);
      const int pw = std::clamp(static_cast<int>(std::lround(w * f)), 1, r.fit_w),
                ph = std::clamp(static_cast<int>(std::lround(h * f)), 1, r.fit_h);
      std::vector<uint8_t> pixels = resize(decoded, w, h, pw, ph);
      free_decoded();   // its memory back before the rectangles'
      cover = std::make_shared<Cover>(picture_rects(pixels.data(), pw, ph, r.scale, r.radius, r.reveal));
    } else {
      std::vector<uint8_t> pixels = square(decoded, w, h, r.size);
      free_decoded();
      cover = std::make_shared<Cover>(cover_rects(pixels.data(), r.size, r.scale, r.radius, r.reveal));
    }
    if (cover->rects.empty()) {   // picture_rects: more rectangles than the memory left holds
      ESP_LOGW(TAG, "%.80s: %dx%d px, too many rectangles for the memory left (PSRAM largest block %u bytes)",
               r.source.c_str(), w, h,
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
      return nullptr;
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
      g_loading = r.source;
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
