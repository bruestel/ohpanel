/*
   The log for the web interface, see logbuf.h.
*/

#include "logbuf.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace logbuf {
namespace {

constexpr size_t kSize = 256 * 1024;   // in PSRAM: days of log, with the status lines every 15 minutes

char *g_ring = nullptr;
uint32_t g_written = 0;  // bytes ever written; the ring holds the last kSize of them
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_serial = nullptr;  // the output before us: the serial console

int hook(const char *fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char line[256];
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n > 0 && g_ring) {
    size_t len = std::min(static_cast<size_t>(n), sizeof(line) - 1);
    // no colour codes in the web interface
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
      if (line[i] == '\033') {
        while (i < len && line[i] != 'm')
          i++;
        continue;
      }
      line[out++] = line[i];
    }
    portENTER_CRITICAL(&g_lock);
    for (size_t i = 0; i < out; i++)
      g_ring[(g_written + i) % kSize] = line[i];
    g_written += out;
    portEXIT_CRITICAL(&g_lock);
  }
  return g_serial ? g_serial(fmt, args) : n;
}

}  // namespace

void begin() {
  if (g_ring)
    return;
  g_ring = static_cast<char *>(heap_caps_malloc_prefer(kSize, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
  if (g_ring)
    g_serial = esp_log_set_vprintf(hook);
}

std::string read(uint32_t since, uint32_t *next, bool *more) {
  constexpr uint32_t kMaxRead = 16 * 1024;  // per request: the copy runs with interrupts off
  *next = 0;
  *more = false;
  if (!g_ring)
    return {};
  // copied under the lock into a buffer made before (no allocation with interrupts off)
  char *buf = static_cast<char *>(heap_caps_malloc_prefer(kMaxRead, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
  if (!buf)
    return {};
  portENTER_CRITICAL(&g_lock);
  const uint32_t end = g_written;
  const uint32_t oldest = end > kSize ? end - kSize : 0;
  const uint32_t from = since < oldest || since > end ? oldest : since;
  const uint32_t to = end - from > kMaxRead ? from + kMaxRead : end;
  for (uint32_t i = from; i < to; i++)
    buf[i - from] = g_ring[i % kSize];
  portEXIT_CRITICAL(&g_lock);
  std::string raw(buf, to - from);
  heap_caps_free(buf);
  if (from == oldest && from > 0 && since != from) {  // the oldest line is cut: start at the next whole one
    const size_t nl = raw.find('\n');
    raw.erase(0, nl == std::string::npos ? raw.size() : nl + 1);
  }
  *next = to;
  *more = to < end;
  return raw;
}

}  // namespace logbuf
