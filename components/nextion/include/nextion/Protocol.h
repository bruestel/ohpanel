/*
   The serial protocol of the Nextion display, as far as the panel uses it.
   Free of ESP-IDF, so it is covered by the host tests.

   Every message from the display ends with FF FF FF. Besides the standard
   ones (startup, ready, touch, errors, get-responses) the display design
   sends two of its own, framed by its event code:

     92 <name> 00 <JSON> 00 FF FF FF    an event, e.g. name "nspanelevent",
                                        JSON {"page", "component", "value"}
     91 <name> 00 <int32 LE> FF FF FF   a value, e.g. a slider position
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nextion {

struct Message {
  enum class Kind {
    Startup,  // 00 00 00: the display (re)booted
    Ready,    // 88: ready for commands
    Touch,    // 65 page component press
    Point,    // 67/68 x y press: a touch anywhere, while sendxy=1
    Event,    // 92 ...: name + json
    Value,    // 91 ...: name + value
    Number,   // 71 int32: answer to "get <number>"
    String,   // 70 text: answer to "get <text>"
    Connect,  // "comok ...": answer to "connect", see DeviceInfo
    Page,     // 66 page: answer to "sendme", the page shown
    Error,    // a single status byte, e.g. 1A invalid variable
    Unknown,
  } kind = Kind::Unknown;

  uint8_t page = 0;       // Touch, Page
  uint8_t component = 0;  // Touch
  bool press = false;     // Touch, Point: true on press, false on release
  int x = 0, y = 0;       // Point
  std::string name;       // Event, Value
  std::string text;       // Event: the JSON; String: the text
  int32_t value = 0;      // Value, Number
  uint8_t code = 0;       // Error, Unknown: the first byte
};

const char *to_string(Message::Kind kind);

/* What the display says about itself in its answer to "connect":
   comok 1,30614-0,NX4832F035_011R,163,61488,D264B8204F0E1828,16777216
         touch,reserved,model,firmware,MCU code,serial,flash bytes */
struct DeviceInfo {
  std::string model;     // "NX4832F035_011R"
  std::string firmware;  // "163"
  std::string mcu;       // "61488"
  std::string serial;    // "D264B8204F0E1828"
  uint32_t flash = 0;    // bytes
  bool touch = false;
};

/* Parses the text after "comok "; false if it has fewer than seven fields. */
bool parse_device_info(const std::string &text, DeviceInfo &out);

/* Splits the byte stream into messages. Input may be cut anywhere. */
class Parser {
 public:
  using Handler = std::function<void(const Message &)>;

  explicit Parser(Handler handler, size_t max_frame = 1024) : handler_(std::move(handler)), max_frame_(max_frame) {}

  void feed(const uint8_t *data, size_t len);

  /* Frames dropped because they grew beyond max_frame without a terminator --
     line noise, or a display that is not speaking this protocol. */
  uint32_t overflows() const { return overflows_; }

 private:
  Handler handler_;
  size_t max_frame_;
  std::string frame_;
  uint32_t overflows_ = 0;

  void dispatch();
};

/* Escapes text for a txt="..." assignment: the display reads \" and \\ (and
   \r as a line break); a raw quote would end the string early. */
std::string quote(const std::string &text);

}  // namespace nextion
