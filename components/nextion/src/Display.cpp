/*
   The Nextion display on its UART.
*/

#include "nextion/Display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace nextion {
namespace {

const char *const TAG = "nextion";

/* The longest message the display sends is an event with its JSON, well under
   this; the snapshot-sized traffic all flows the other way. */
constexpr size_t kRxBuffer = 2048;
constexpr size_t kTxBuffer = 2048;

/* Flow control (see the header): sync at least every this many bytes -- half
   the display's 1 KB input buffer --, wait this long for the answer, and stop
   asking after this many unanswered syncs in a row. */
constexpr size_t kSyncBytes = 512;
constexpr uint32_t kSyncTimeoutMs = 500;
constexpr int kSyncGiveUp = 3;

/* The rates a display may run at besides the design's: 115200 (an older
   design of ours), 9600 (the Nextion default: another design, or one without
   a valid design). */
constexpr int kOtherRates[] = {115200, 9600};

}  // namespace

bool Display::begin(const Config &cfg, Handler handler) {
  cfg_ = cfg;
  handler_ = std::move(handler);
  mutex_ = xSemaphoreCreateMutex();
  sync_ = xSemaphoreCreateBinary();

  uart_config_t uc = {};
  uc.baud_rate = cfg.baud;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(cfg.port, kRxBuffer, kTxBuffer, 0, nullptr, 0) != ESP_OK ||
      uart_param_config(cfg.port, &uc) != ESP_OK ||
      uart_set_pin(cfg.port, cfg.tx, cfg.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGE(TAG, "UART setup failed");
    return false;
  }
  if (xTaskCreate(reader, "nextion_rx", 4096, this, 6, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "reader task failed");
    return false;
  }
  baud_ = cfg.baud;
  ESP_LOGI(TAG, "Display on UART%d (TX %d, RX %d) at %d baud", cfg.port, cfg.tx, cfg.rx, cfg.baud);
  return true;
}

int Display::locate() {
  if (uploading_.load() || !mutex_)
    return 0;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const int found = locate_locked();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return found;
}

bool Display::ping() {
  if (uploading_.load() || !mutex_)
    return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool ok = ping_locked(300);
  if (ok)
    sync_misses_ = 0;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ok;
}

bool Display::lost() const { return sync_misses_ >= kSyncGiveUp; }

/* The rate in use first, then the design's, then the others. Called with the
   mutex held. */
int Display::locate_locked() {
  std::vector<int> rates = {baud_, cfg_.baud};
  for (const int r : kOtherRates)
    rates.push_back(r);
  for (size_t i = 0; i < rates.size(); i++) {
    if (std::find(rates.begin(), rates.begin() + i, rates[i]) != rates.begin() + i)
      continue;   // tried already
    if (rates[i] != baud_) {
      uart_set_baudrate(cfg_.port, rates[i]);
      baud_ = rates[i];
    }
    if (ping_locked(200)) {
      sync_misses_ = 0;
      return baud_;
    }
  }
  return 0;
}

void Display::send(const std::string &command) {
  static const char kEnd[3] = {'\xFF', '\xFF', '\xFF'};
  if (uploading_.load())
    return;  // the display is being rewritten; nothing it could show anyway
  if (!mutex_)
    return;  // begin() has not run (or failed): no UART to write to
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  // pausing the refresh while the display still draws cuts that drawing off
  if (command == "ref_stop")
    sync_locked();
  uart_write_bytes(cfg_.port, command.data(), command.size());
  uart_write_bytes(cfg_.port, kEnd, sizeof(kEnd));
  bytes_sent_ += command.size() + 3;
  since_sync_ += command.size() + 3;
  // a page change keeps the display busy drawing; so does a full redraw
  const bool redraw = command.rfind("page ", 0) == 0 || command == "ref 0" || command == "ref_star" || command == "doevents";
  if (redraw || since_sync_ >= kSyncBytes)
    sync_locked();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  if (log_commands_.load())
    ESP_LOGI(TAG, "> %s", command.c_str());
  else
    ESP_LOGD(TAG, "> %s", command.c_str());
}

/* Waits until the display has worked through everything sent so far (see the
   header). Called with the mutex held. */
void Display::sync_locked() {
  since_sync_ = 0;
  if (!sync_ || sync_misses_ >= kSyncGiveUp)
    return;  // not answering: do not stall every command; an answer re-arms it (reader)
  auto *sem = static_cast<SemaphoreHandle_t>(sync_);
  xSemaphoreTake(sem, 0);  // drop an answer left over from before
  static const char kSendme[] = "sendme\xFF\xFF\xFF";
  uart_write_bytes(cfg_.port, kSendme, sizeof(kSendme) - 1);
  if (xSemaphoreTake(sem, pdMS_TO_TICKS(kSyncTimeoutMs)) == pdTRUE)
    sync_misses_ = 0;
  else if (++sync_misses_ == kSyncGiveUp)
    ESP_LOGW(TAG, "display does not answer \"sendme\" at %d baud", baud_);
}

/* Asks "sendme" and waits for the answer. The leading FF FF FF ends whatever
   half command sits in the display's buffer -- bytes sent at a rate it does
   not run at arrive as such junk, and a command appended to it is lost. */
bool Display::ping_locked(uint32_t timeout_ms) {
  auto *sem = static_cast<SemaphoreHandle_t>(sync_);
  xSemaphoreTake(sem, 0);
  static const char kSendme[] = "\xFF\xFF\xFFsendme\xFF\xFF\xFF";
  uart_write_bytes(cfg_.port, kSendme, sizeof(kSendme) - 1);
  return xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}


void Display::reader(void *arg) {
  auto *self = static_cast<Display *>(arg);
  Parser parser([self](const Message &m) {
    self->messages_++;
    if (m.kind == Message::Kind::Error)
      ESP_LOGW(TAG, "display reports status 0x%02X", m.code);
    if (m.kind == Message::Kind::Page) {
      self->sync_misses_ = 0;  // answering again
      xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->sync_));
    }
    if (self->handler_)
      self->handler_(m);
  });
  uint8_t buf[128];
  for (;;) {
    if (self->uploading_.load()) {  // the upload reads the confirmations itself
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const int n = uart_read_bytes(self->cfg_.port, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (n > 0)
      parser.feed(buf, static_cast<size_t>(n));
  }
}


// ------------------------------------------------------------------ upload --

namespace {
constexpr size_t kBlock = 4096;
/* The display erases flash as it goes; a block can take a while to confirm. */
constexpr uint32_t kAckTimeoutMs = 5000;

void raw(uart_port_t port, const std::string &command) {
  static const char kEnd[3] = {'\xFF', '\xFF', '\xFF'};
  uart_write_bytes(port, command.data(), command.size());
  uart_write_bytes(port, kEnd, sizeof(kEnd));
}
}  // namespace

bool Display::wait_ack(uint32_t timeout_ms, std::string *error) {
  uint8_t b = 0;
  const int n = uart_read_bytes(cfg_.port, &b, 1, pdMS_TO_TICKS(timeout_ms));
  if (n == 1) {
    // 0x05: block accepted. The proven path treated any byte as the go-ahead; so does this,
    // but it says so when the display answers something else.
    if (b != 0x05)
      ESP_LOGW(TAG, "upload: display answered 0x%02X instead of 0x05", b);
    return true;
  }
  if (error)
    *error = "the display did not confirm (timeout after " + std::to_string(upload_done_) + " bytes)";
  return false;
}


bool Display::upload_begin(size_t size, int baud, std::string *error) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (uploading_.load()) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    if (error) *error = "an upload is already running";
    return false;
  }
  locate_locked();   // the rate it answers at, if any (none: a display without a valid design answers little)
  uploading_.store(true);
  block_ = static_cast<uint8_t *>(malloc(kBlock));
  if (!block_) {
    if (error) *error = "out of memory";
    upload_finish();
    return false;
  }
  block_fill_ = 0;
  upload_size_ = size;
  upload_done_ = 0;
  vTaskDelay(pdMS_TO_TICKS(50));  // let the reader task step aside

  ESP_LOGW(TAG, "Uploading a display firmware: %u bytes at %d baud", static_cast<unsigned>(size), baud);
  /* The upload command at the rate the display was found at; where it does
     not confirm, at the others it may be at unnoticed -- a display without a
     valid design (an upload broken off) answers little, and may sit at a rate
     it was told and could not confirm. */
  std::vector<int> rates;
  for (const int r : {baud_, cfg_.baud, baud, kOtherRates[0], kOtherRates[1]})
    if (std::find(rates.begin(), rates.end(), r) == rates.end())
      rates.push_back(r);
  for (size_t i = 0; i < rates.size(); i++) {
    uart_set_baudrate(cfg_.port, rates[i]);
    raw(cfg_.port, "");
    raw(cfg_.port, "connect");
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_flush_input(cfg_.port);  // the "comok ..." answer
    raw(cfg_.port, "");
    raw(cfg_.port, "whmi-wri " + std::to_string(size) + "," + std::to_string(baud) + ",res0");
    uart_wait_tx_done(cfg_.port, pdMS_TO_TICKS(500));
    if (baud != rates[i]) {
      // the display answers at the new rate already
      vTaskDelay(pdMS_TO_TICKS(50));
      uart_set_baudrate(cfg_.port, baud);
    }
    std::string why;
    if (wait_ack(i + 1 < rates.size() ? 2500 : kAckTimeoutMs, &why)) {
      if (i > 0)
        ESP_LOGW(TAG, "the display took the upload at %d baud", rates[i]);
      baud_ = baud;
      return true;
    }
    if (i + 1 == rates.size()) {
      if (error) *error = why;
      uart_set_baudrate(cfg_.port, baud_);
      upload_finish();
      return false;
    }
    ESP_LOGW(TAG, "no answer to the upload at %d baud -- trying %d", rates[i], rates[i + 1]);
  }
  return false;
}

bool Display::upload_write(const uint8_t *data, size_t len, std::string *error) {
  if (!uploading_.load() || !block_) {
    if (error) *error = "no upload running";
    return false;
  }
  while (len > 0) {
    const size_t take = std::min(len, kBlock - block_fill_);
    memcpy(block_ + block_fill_, data, take);
    block_fill_ += take;
    data += take;
    len -= take;
    if (block_fill_ == kBlock) {
      uart_write_bytes(cfg_.port, reinterpret_cast<const char *>(block_), kBlock);
      upload_done_ += kBlock;
      block_fill_ = 0;
      // the last block is confirmed in upload_end, by the display rebooting
      if (upload_done_ < upload_size_ && !wait_ack(kAckTimeoutMs, error)) {
        upload_finish();
        return false;
      }
    }
  }
  return true;
}

bool Display::upload_end(std::string *error) {
  if (!uploading_.load() || !block_) {
    if (error) *error = "no upload running";
    return false;
  }
  if (block_fill_ > 0) {
    uart_write_bytes(cfg_.port, reinterpret_cast<const char *>(block_), block_fill_);
    upload_done_ += block_fill_;
  }
  uart_wait_tx_done(cfg_.port, pdMS_TO_TICKS(2000));
  const bool complete = upload_done_ == upload_size_;
  // after the last byte the display checks the file and reboots; a final confirmation may come or not
  std::string ignored;
  wait_ack(2000, &ignored);
  ESP_LOGW(TAG, "Display firmware %s (%u of %u bytes)", complete ? "written" : "incomplete",
           static_cast<unsigned>(upload_done_), static_cast<unsigned>(upload_size_));
  upload_finish();
  if (!complete && error)
    *error = "the file ended early";
  return complete;
}

void Display::upload_abort() {
  if (uploading_.load())
    upload_finish();
}

void Display::upload_finish() {
  free(block_);
  block_ = nullptr;
  // the display restarts with its design's rate (ours: Config::baud)
  uart_set_baudrate(cfg_.port, cfg_.baud);
  baud_ = cfg_.baud;
  uart_flush_input(cfg_.port);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  uploading_.store(false);
}

}  // namespace nextion
