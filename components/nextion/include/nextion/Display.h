/*
   The Nextion display on its UART.

   Commands are plain text ending in FF FF FF; send() adds the terminator and
   may be called from any task.

   Flow control: the display reads into a 1 KB buffer and stops reading while
   it draws a page; what overflows is lost (it reports 0x24). So after a page
   change and after every 512 bytes, send() asks "sendme" and waits for the
   answer, which the display only gives once it has worked through everything
   before it. A display that does not answer is not waited for again until it
   does (lost()). A reader task parses what the display sends and hands every
   message to the handler -- on that task, so handlers keep it short.

   The driver only carries bytes: finding the display, noticing that it
   restarted and drawing again are the caller's (appdisplay, its link).
*/

#pragma once

#include "nextion/Protocol.h"

#include <driver/gpio.h>
#include <driver/uart.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nextion {

struct Config {
  uart_port_t port = UART_NUM_2;
  gpio_num_t tx = GPIO_NUM_16;
  gpio_num_t rx = GPIO_NUM_17;
  int baud = 921600;   // the rate the design runs at (its Program.s); locate() also tries the usual others
};

class Display {
 public:
  using Handler = std::function<void(const Message &)>;

  bool begin(const Config &cfg, Handler handler);

  void send(const std::string &command);
  /* Every command sent logged at INFO (for the web interface's log). */
  void log_commands(bool on) { log_commands_ = on; }
  bool logging_commands() const { return log_commands_.load(); }

  /* Shorthands for the assignments the panel needs. */
  void txt(const std::string &obj, const std::string &text) { send(obj + ".txt=\"" + quote(text) + "\""); }
  void val(const std::string &obj, int value) { send(obj + ".val=" + std::to_string(value)); }
  void set(const std::string &obj, const char *attr, int value) {
    send(obj + "." + attr + "=" + std::to_string(value));
  }
  void vis(const std::string &obj, bool visible) { send("vis " + obj + "," + (visible ? "1" : "0")); }
  void page(int id) { send("page " + std::to_string(id)); }
  void dim(int percent) { send("dim=" + std::to_string(percent)); }

  /* Writing a display firmware (.tft), Nextion upload protocol: "whmi-wri
     <size>,<baud>,res0", then the file in 4096-byte blocks, each confirmed by
     the display. While an upload runs the driver is exclusive to it: the
     reader stops and send() drops commands. The display reboots afterwards
     (a Startup message follows). `baud` may be raised for the transfer
     (115200 is the proven rate, 921600 is four times faster). */
  bool upload_begin(size_t size, int baud, std::string *error);
  bool upload_write(const uint8_t *data, size_t len, std::string *error);
  bool upload_end(std::string *error);
  void upload_abort();
  bool uploading() const { return uploading_.load(); }

  uint32_t bytes_sent() const { return bytes_sent_; }

  /* Looks for the display at the rates it may run at -- Config::baud (the
     design's), then 115200 and 9600 (an older or another design, a display
     without a valid one) -- and keeps the link at the one it answers at.
     Returns that rate, 0 if it answers at none. */
  int locate();
  /* Whether it answers at the rate in use. */
  bool ping();
  /* It stopped answering the flow control (several "sendme" in a row): the
     link is lost until locate() finds it again. */
  bool lost() const;
  int baud() const { return baud_; }
  uint32_t messages_received() const { return messages_; }

 private:
  Config cfg_;
  Handler handler_;
  void *mutex_ = nullptr;
  void *sync_ = nullptr;         // given by the reader on every "sendme" answer
  size_t since_sync_ = 0;        // bytes sent since the last sync
  std::atomic<int> sync_misses_{0};  // syncs in a row the display did not answer
  int baud_ = 921600;                // the link's rate now
  void sync_locked();
  bool ping_locked(uint32_t timeout_ms);
  int locate_locked();
  uint32_t bytes_sent_ = 0;
  uint32_t messages_ = 0;
  std::atomic<bool> uploading_{false};
  std::atomic<bool> log_commands_{false};
  uint8_t *block_ = nullptr;
  size_t block_fill_ = 0;
  size_t upload_size_ = 0;
  size_t upload_done_ = 0;

  bool wait_ack(uint32_t timeout_ms, std::string *error);
  void upload_finish();

  static void reader(void *self);
};

}  // namespace nextion
