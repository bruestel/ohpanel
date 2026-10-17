/*
   Server-sent events parser (https://html.spec.whatwg.org/#event-stream-interpretation),
   reduced to what openHAB sends. Free of ESP-IDF so it can be tested on a host.
*/

#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace appoh {

/* Line by line: "event:" names the event, "data:" lines carry it (joined with
   newlines), a blank line dispatches it. Comments (":...") and "id:" are
   ignored. Input may be split anywhere, also inside a line or a CRLF. */
class SseParser {
 public:
  using Handler = std::function<void(const std::string &event, const std::string &data)>;

  SseParser(size_t max_line, Handler handler) : max_line_(max_line), handler_(std::move(handler)) {}

  /* False once a line grew beyond max_line; the parser is unusable then. */
  bool feed(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
      const char c = buf[i];
      if (c == '\n') {
        if (!line_.empty() && line_.back() == '\r')
          line_.pop_back();
        on_line();
        line_.clear();
      } else {
        line_ += c;
        if (line_.size() > max_line_)
          return false;
      }
    }
    return true;
  }

 private:
  size_t max_line_;
  Handler handler_;
  std::string line_;
  std::string event_;
  std::string data_;
  bool has_data_ = false;

  void on_line() {
    if (line_.empty()) {
      if (has_data_ || !event_.empty())
        handler_(event_, data_);
      event_.clear();
      data_.clear();
      has_data_ = false;
      return;
    }
    if (line_[0] == ':')
      return;
    const size_t colon = line_.find(':');
    const std::string field = line_.substr(0, colon);
    std::string value = colon == std::string::npos ? std::string() : line_.substr(colon + 1);
    if (!value.empty() && value[0] == ' ')
      value.erase(0, 1);
    if (field == "event") {
      event_ = value;
    } else if (field == "data") {
      if (has_data_)
        data_ += '\n';
      data_ += value;
      has_data_ = true;
    }
  }
};

}  // namespace appoh
