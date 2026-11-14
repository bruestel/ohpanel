/*
   openHAB's state stream read as it comes (StateStream.h).
*/

#include "StateStream.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace appoh {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr size_t kMaxKey = 256;        // an item's name, a member's
constexpr size_t kMaxShort = 64;       // type, unit
constexpr size_t kMaxField = 32;       // an event stream's field name
constexpr size_t kMaxEvent = 64;       // its event name
constexpr size_t kMaxText = 4096;      // the data of an event that is no state event
constexpr size_t kMaxLiteral = 64;     // a number

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

std::string rest_state_path(const std::string &item, uint64_t version) {
  char v[17];
  std::snprintf(v, sizeof(v), "%016llx", static_cast<unsigned long long>(version));
  return "/rest/items/" + item + "/state?v=" + v;
}

void finish_state(ItemState &s, bool display_given, const char *numeric_text) {
  if (!display_given)
    s.display = s.state;
  if (numeric_text) {
    char *end = nullptr;
    const double v = std::strtod(numeric_text, &end);
    if (end != numeric_text && *end == '\0' && std::isfinite(v)) {
      s.numeric = v;
      s.has_numeric = true;
      return;
    }
  }
  if (s.type == "Decimal" || s.type == "Quantity" || s.type == "Percent") {
    /* An openHAB that sends no numericState and no unit (older ones): the number read from the state, which
       openHAB writes with a dot and the unit after a space ("8.6 °C"), and a quantity's unit from there too --
       else the panel showed the display text as it was ("8.6 °C" where "9°" belongs: no decimals, no rounding;
       forum #124), took a colour temperature in Kelvin for percent and sent a thermostat's target without its
       unit. */
    // openHAB's form only: a decimal number (no hex, no space before it), after it nothing but a quantity's unit
    const char *text = s.state.c_str();
    const bool starts = std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' || text[0] == '+' ||
                        text[0] == '.';
    char *end = nullptr;
    const double v = starts ? std::strtod(text, &end) : 0;
    std::string unit = end ? end : "";
    unit.erase(0, unit.find_first_not_of(' '));
    unit.erase(unit.find_last_not_of(' ') + 1);
    const bool hex = s.state.find_first_of("xX") != std::string::npos && unit.empty();
    if (starts && end != text && std::isfinite(v) && !hex && (s.type == "Quantity" || unit.empty())) {
      s.numeric = v;
      s.has_numeric = true;
      if (s.type == "Quantity")
        s.unit = unit;   // the number's own, also where openHAB sent another one (that one belongs to no number here)
    }
  }
}

// ------------------------------------------------------------ event stream ----

void StateStream::feed(const char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = buf[i];
    if (c == '\r')   // CRLF as LF (openHAB sends no lone CR; JSON holds none unescaped)
      continue;
    switch (line_) {
      case Line::Start:
        if (c == '\n') {
          end_event();
        } else if (c == ':') {
          line_ = Line::Skip;   // a comment
        } else {
          field_.assign(1, c);
          line_ = Line::Field;
        }
        break;
      case Line::Field:
        if (c == '\n') {
          end_line();
        } else if (c == ':') {
          line_ = Line::Value;
          space_ = true;
          if (field_ == "event") {
            event_.clear();
          } else if (field_ == "data") {
            if (data_seen_) {
              value_char('\n');   // data lines are joined with a newline
            } else {
              data_seen_ = true;
              json_ = event_.empty() || event_ == "message";
              json_reset();
            }
          }
        } else if (field_.size() < kMaxField) {
          field_ += c;
        }
        break;
      case Line::Value:
        if (c == '\n') {
          end_line();
        } else {
          if (!(space_ && c == ' '))
            value_char(c);
          space_ = false;
        }
        break;
      case Line::Skip:
        if (c == '\n')
          line_ = Line::Start;
        break;
    }
  }
}

void StateStream::value_char(char c) {
  if (field_ == "event") {
    if (event_.size() < kMaxEvent)
      event_ += c;
  } else if (field_ == "data") {
    if (json_)
      json_char(c);
    else if (text_.size() < kMaxText)
      text_ += c;
  }
}

void StateStream::end_line() {
  if (line_ == Line::Field) {   // a field without a colon: its value is empty
    if (field_ == "event") {
      event_.clear();
    } else if (field_ == "data" && !data_seen_) {
      data_seen_ = true;
      json_ = event_.empty() || event_ == "message";
      json_reset();
    }
  }
  line_ = Line::Start;
  field_.clear();
}

void StateStream::end_event() {
  if (data_seen_ && json_) {
    const bool empty = j_ == J::Value && nest_.empty();
    if (!empty && !(j_ == J::After && nest_.empty()))
      bad_events_++;
  } else if (data_seen_ || !event_.empty()) {
    if (on_event_)
      on_event_(event_, text_);
  }
  event_.clear();
  text_.clear();
  data_seen_ = false;
  json_ = false;
  json_reset();
}

// -------------------------------------------------------------------- JSON ----

void StateStream::json_reset() {
  j_ = J::Value;
  nest_.clear();
  str_.clear();
  str_.shrink_to_fit();   // a picture's text: not kept until the next one
  literal_.clear();
  item_.clear();
  member_.clear();
}

void StateStream::json_char(char c) {
  switch (j_) {
    case J::Fail:
      return;
    case J::String:
      string_byte(c);
      return;
    case J::Literal:
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '+' || c == '-') {
        if (literal_.size() < kMaxLiteral)
          literal_ += c;
        return;
      }
      end_literal();
      json_char(c);   // what ended it: a comma, a bracket, a space
      return;
    default:
      break;
  }
  if (space(c))
    return;
  switch (j_) {
    case J::Value:
      if (c == '{' || c == '[') {
        open(c);
      } else if (c == ']' && !nest_.empty() && nest_.back() == '[') {
        close(c);   // an empty list
      } else if (c == '"') {
        begin_string(false);
      } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) || c == 't' || c == 'f' || c == 'n') {
        if (nest_.empty()) {
          j_ = J::Fail;   // a state event is an object
          return;
        }
        literal_.assign(1, c);
        j_ = J::Literal;
      } else {
        j_ = J::Fail;
      }
      break;
    case J::Key:
      if (c == '"')
        begin_string(true);
      else if (c == '}')
        close(c);   // an empty object
      else
        j_ = J::Fail;
      break;
    case J::Colon:
      j_ = c == ':' ? J::Value : J::Fail;
      break;
    case J::After:
      if (nest_.empty())
        j_ = J::Fail;   // something after the object
      else if (c == ',')
        j_ = nest_.back() == '{' ? J::Key : J::Value;
      else if (c == '}' || c == ']')
        close(c);
      else
        j_ = J::Fail;
      break;
    default:
      break;
  }
}

void StateStream::open(char c) {
  if (nest_.empty() && c != '{') {
    j_ = J::Fail;
    return;
  }
  nest_.push_back(c);
  if (nest_.size() == 2 && c == '{') {   // an item's object
    st_ = ItemState{};
    display_given_ = state_large_ = false;
    display_large_ = false;
    state_len_ = 0;
    state_hash_ = 0;
    numeric_.clear();
  }
  j_ = c == '{' ? J::Key : J::Value;
}

void StateStream::close(char c) {
  if (nest_.empty() || nest_.back() != (c == '}' ? '{' : '[')) {
    j_ = J::Fail;
    return;
  }
  if (nest_.size() == 2 && c == '}' && !item_.empty()) {
    ItemState s = std::move(st_);
    const bool picture = s.type == "Raw";
    /* A picture: openHAB's stream gives it as "raw type (image/jpeg): 219955 bytes" (5.x), or as itself
       ("data:image/...;base64,...", where an openHAB does): kept as it is only that way and small, else by its
       address on openHAB. That text tells no new picture of the same size from the old one: its address counts on
       with each state that comes; the picture itself is told by its hash. */
    const bool inline_picture = picture && !state_large_ && state_len_ <= max_picture_ && s.state.rfind("data:", 0) == 0;
    if (state_large_ || (picture && !inline_picture)) {
      // too large to keep: a picture by its address on openHAB, anything else left out
      size_t bytes = state_len_;
      uint64_t version = state_hash_;
      if (picture && !state_large_ && s.state.rfind("data:", 0) != 0) {
        const size_t colon = s.state.rfind(": ");
        bytes = colon == std::string::npos ? 0 : std::strtoul(s.state.c_str() + colon + 2, nullptr, 10);
        version ^= ++counts_[item_] * kFnvPrime;
      }
      if (on_large_)
        on_large_(item_, bytes, picture);
      if (picture) {
        s.state = s.display = rest_state_path(item_, version);
        if (on_item_)
          on_item_(item_, s);
      }
    } else {
      finish_state(s, display_given_ && !display_large_, numeric_.empty() ? nullptr : numeric_.c_str());
      if (on_item_)
        on_item_(item_, s);
    }
    st_ = ItemState{};
  }
  nest_.pop_back();
  j_ = J::After;
}

void StateStream::begin_string(bool key) {
  key_ = key;
  str_.clear();
  str_len_ = 0;
  hash_ = kFnvOffset;
  esc_ = false;
  hex_left_ = 0;
  high_ = 0;
  if (key) {
    cap_ = kMaxKey;
  } else if (nest_.size() == 2 && (member_ == "state" || member_ == "displayState")) {
    cap_ = max_text_;
  } else if (nest_.size() == 2 && (member_ == "type" || member_ == "unit")) {
    cap_ = kMaxShort;
  } else {
    cap_ = 0;   // nothing the panel reads
  }
  j_ = J::String;
}

void StateStream::string_byte(char c) {
  if (hex_left_ > 0) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else {
      j_ = J::Fail;
      return;
    }
    hex_ = hex_ << 4 | static_cast<uint32_t>(v);
    if (--hex_left_ == 0) {
      const uint32_t cp = hex_;
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (high_)
          string_utf8(0xFFFD);
        high_ = cp;   // its low surrogate follows
      } else if (cp >= 0xDC00 && cp <= 0xDFFF && high_) {
        string_utf8(0x10000 + ((high_ - 0xD800) << 10) + (cp - 0xDC00));
        high_ = 0;
      } else {
        if (high_)
          string_utf8(0xFFFD);
        high_ = 0;
        string_utf8(cp >= 0xD800 && cp <= 0xDFFF ? 0xFFFD : cp);
      }
    }
    return;
  }
  if (esc_) {
    esc_ = false;
    if (c == 'u') {
      hex_left_ = 4;
      hex_ = 0;
      return;
    }
    char out;
    switch (c) {
      case '"': case '\\': case '/': out = c; break;
      case 'b': out = '\b'; break;
      case 'f': out = '\f'; break;
      case 'n': out = '\n'; break;
      case 'r': out = '\r'; break;
      case 't': out = '\t'; break;
      default:
        j_ = J::Fail;
        return;
    }
    if (high_) {
      string_utf8(0xFFFD);
      high_ = 0;
    }
    string_utf8(static_cast<unsigned char>(out));
    return;
  }
  if (high_ && c != '\\') {   // a high surrogate without its low one
    string_utf8(0xFFFD);
    high_ = 0;
  }
  if (c == '\\') {
    esc_ = true;
  } else if (c == '"') {
    if (high_) {
      string_utf8(0xFFFD);
      high_ = 0;
    }
    end_string();
  } else {
    str_len_++;
    hash_ = (hash_ ^ static_cast<unsigned char>(c)) * kFnvPrime;
    if (str_.size() < cap_)
      str_ += c;
  }
}

void StateStream::string_utf8(uint32_t cp) {
  char b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    b[0] = static_cast<char>(0xC0 | cp >> 6);
    b[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = static_cast<char>(0xE0 | cp >> 12);
    b[1] = static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    b[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    b[0] = static_cast<char>(0xF0 | cp >> 18);
    b[1] = static_cast<char>(0x80 | (cp >> 12 & 0x3F));
    b[2] = static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    b[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  for (size_t i = 0; i < n; i++) {
    str_len_++;
    hash_ = (hash_ ^ static_cast<unsigned char>(b[i])) * kFnvPrime;
    if (str_.size() < cap_)
      str_ += b[i];
  }
}

void StateStream::end_string() {
  if (key_) {
    if (nest_.size() == 1)
      item_ = str_;
    else if (nest_.size() == 2)
      member_ = str_;
    j_ = J::Colon;
    return;
  }
  if (nest_.size() == 2) {
    const bool over = str_len_ > cap_;
    if (member_ == "state") {
      state_large_ = over;
      state_len_ = str_len_;
      state_hash_ = hash_;
      if (!over)
        st_.state = std::move(str_);
    } else if (member_ == "displayState") {
      display_given_ = true;
      display_large_ = over;
      if (!over)
        st_.display = std::move(str_);
    } else if (member_ == "type" && !over) {
      st_.type = std::move(str_);
    } else if (member_ == "unit" && !over) {
      st_.unit = std::move(str_);
    }
  }
  str_.clear();
  if (str_.capacity() > 4096)
    str_.shrink_to_fit();   // a picture's text: not kept until the next one
  j_ = J::After;
}

void StateStream::end_literal() {
  if (nest_.size() == 2 && member_ == "numericState" && literal_ != "null" && literal_ != "true" &&
      literal_ != "false")
    numeric_ = literal_;
  literal_.clear();
  j_ = J::After;
}

}  // namespace appoh
