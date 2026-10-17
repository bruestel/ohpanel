/*
   The serial protocol of the Nextion display.
*/

#include "nextion/Protocol.h"

#include <cstdlib>
#include <cstring>

namespace nextion {

const char *to_string(Message::Kind kind) {
  switch (kind) {
    case Message::Kind::Startup: return "startup";
    case Message::Kind::Ready: return "ready";
    case Message::Kind::Touch: return "touch";
    case Message::Kind::Point: return "point";
    case Message::Kind::Event: return "event";
    case Message::Kind::Value: return "value";
    case Message::Kind::Number: return "number";
    case Message::Kind::String: return "string";
    case Message::Kind::Connect: return "connect";
    case Message::Kind::Page: return "page";
    case Message::Kind::Error: return "error";
    case Message::Kind::Unknown: return "unknown";
  }
  return "?";
}

void Parser::feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    frame_ += static_cast<char>(data[i]);
    const size_t n = frame_.size();
    if (n >= 3 && static_cast<uint8_t>(frame_[n - 1]) == 0xFF && static_cast<uint8_t>(frame_[n - 2]) == 0xFF &&
        static_cast<uint8_t>(frame_[n - 3]) == 0xFF) {
      frame_.resize(n - 3);
      dispatch();
      frame_.clear();
    } else if (n > max_frame_) {
      overflows_++;
      frame_.clear();
    }
  }
}

void Parser::dispatch() {
  Message m;
  const auto *b = reinterpret_cast<const uint8_t *>(frame_.data());
  const size_t n = frame_.size();
  if (n == 0)
    return;
  m.code = b[0];

  // A name terminated by 00, starting at offset 1; returns the offset after it or 0.
  const auto name_end = [&]() -> size_t {
    const void *zero = std::memchr(b + 1, 0, n - 1);
    return zero ? static_cast<const uint8_t *>(zero) - b : 0;
  };

  if (n == 3 && b[0] == 0 && b[1] == 0 && b[2] == 0) {
    m.kind = Message::Kind::Startup;
  } else if (n == 1 && b[0] == 0x88) {
    m.kind = Message::Kind::Ready;
  } else if (n == 4 && b[0] == 0x65) {
    m.kind = Message::Kind::Touch;
    m.page = b[1];
    m.component = b[2];
    m.press = b[3] == 0x01;
  } else if (n == 6 && (b[0] == 0x67 || b[0] == 0x68)) {
    m.kind = Message::Kind::Point;
    m.x = b[1] << 8 | b[2];
    m.y = b[3] << 8 | b[4];
    m.press = b[5] == 0x01;
  } else if (b[0] == 0x92 && n > 2) {
    const size_t end = name_end();
    if (end) {
      m.kind = Message::Kind::Event;
      m.name.assign(frame_, 1, end - 1);
      m.text.assign(frame_, end + 1, std::string::npos);
      // the JSON is closed by a 00 of its own
      if (!m.text.empty() && m.text.back() == '\0')
        m.text.pop_back();
    }
  } else if (b[0] == 0x91 && n > 2) {
    const size_t end = name_end();
    if (end && n - end - 1 >= 4) {
      m.kind = Message::Kind::Value;
      m.name.assign(frame_, 1, end - 1);
      const uint8_t *v = b + end + 1;
      m.value = static_cast<int32_t>(v[0] | (v[1] << 8) | (v[2] << 16) | (static_cast<uint32_t>(v[3]) << 24));
    }
  } else if (b[0] == 0x71 && n == 5) {
    m.kind = Message::Kind::Number;
    m.value = static_cast<int32_t>(b[1] | (b[2] << 8) | (b[3] << 16) | (static_cast<uint32_t>(b[4]) << 24));
  } else if (n == 2 && b[0] == 0x66) {
    m.kind = Message::Kind::Page;
    m.page = b[1];
  } else if (n > 6 && frame_.compare(0, 6, "comok ") == 0) {
    m.kind = Message::Kind::Connect;
    m.text.assign(frame_, 6, std::string::npos);
  } else if (b[0] == 0x70) {
    m.kind = Message::Kind::String;
    m.text.assign(frame_, 1, std::string::npos);
  } else if (n == 1) {
    m.kind = Message::Kind::Error;
  }
  handler_(m);
}

bool parse_device_info(const std::string &text, DeviceInfo &out) {
  std::string fields[7];
  size_t count = 0, start = 0;
  while (count < 7) {
    const size_t comma = text.find(',', start);
    fields[count++] = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  if (count < 7)
    return false;
  out.touch = fields[0] == "1";
  out.model = fields[2];
  out.firmware = fields[3];
  out.mcu = fields[4];
  out.serial = fields[5];
  out.flash = static_cast<uint32_t>(std::strtoul(fields[6].c_str(), nullptr, 10));
  return true;
}

std::string quote(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (c == '\n') {
      out += "\\r";
      continue;
    }
    out += c;
  }
  return out;
}

}  // namespace nextion
