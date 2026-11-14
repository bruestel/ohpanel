/*
   openHAB's state stream (/rest/events/states) read as it comes: the server-sent events
   (https://html.spec.whatwg.org/#event-stream-interpretation, reduced to what openHAB sends) and, in them, the
   states' JSON item by item -- nothing held whole, neither a line nor an event. The snapshot after subscribing
   carries every item in one line, a change one. A picture (an Image item) comes as "raw type (image/jpeg): N
   bytes" (openHAB 5) or as itself ("data:image/...;base64,...", hundreds of KB): only a small one of those stays
   as it is, any other as its address on openHAB (rest_state_path), fetched when it is to be shown. A value too
   large to keep is read past, never held; anything but a picture that large is left out, and said so.
   Free of ESP-IDF so it can be tested on a host.
*/

#pragma once

#include "appoh/OpenHab.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace appoh {

/* Where openHAB gives an item's state on its own (GET, the state as text), with `version` -- a hash of that state --
   in the query, so a new picture is a new address and the same one is not fetched again. */
std::string rest_state_path(const std::string &item, uint64_t version);

/* An item's state from what its JSON object held; `display_given`: displayState was there (openHAB leaves it out
   whenever it would equal the state). The number of a number's state also where openHAB sent no numericState
   (older ones, forum #124). */
void finish_state(ItemState &s, bool display_given, const char *numeric_text);

class StateStream {
 public:
  using ItemHandler = std::function<void(const std::string &item, const ItemState &state)>;
  /* An event that is no state event ("ready" with the connection id, "alive"), its data joined. */
  using EventHandler = std::function<void(const std::string &event, const std::string &data)>;
  /* A state read past: the item, how large it was, whether it is kept as an address (a picture) or left out. */
  using LargeHandler = std::function<void(const std::string &item, size_t bytes, bool as_address)>;

  /* `max_text`: the longest state (or display text) kept as it is; `max_picture`: a picture's (type Raw) up to
     which it is kept as it is too, a larger one becomes rest_state_path. */
  StateStream(size_t max_text, size_t max_picture, ItemHandler on_item, EventHandler on_event,
              LargeHandler on_large = {})
      : max_text_(max_text), max_picture_(max_picture), on_item_(std::move(on_item)), on_event_(std::move(on_event)),
        on_large_(std::move(on_large)) {}

  /* Input may be split anywhere, also inside a line, a CRLF or a character. */
  void feed(const char *buf, size_t len);

  /* State events whose JSON was not states (each then ends at its blank line, the stream goes on). */
  int bad_events() const { return bad_events_; }

 private:
  // ---- the event stream
  enum class Line { Start, Field, Value, Skip };
  Line line_ = Line::Start;
  std::string field_;          // the line's field name, so far
  std::string event_;          // "event:"
  std::string text_;           // data of an event that is no state event (short: a connection id)
  bool data_seen_ = false;     // a "data:" line in this event
  bool json_ = false;          // its data goes to the JSON reader
  bool space_ = false;         // the value's first character still to come (one space after the colon is dropped)
  void end_line();
  void end_event();
  void value_char(char c);

  // ---- the JSON in a state event: {"Item": {"state": ..., ...}, ...}
  enum class J { Value, Key, Colon, After, String, Literal, Fail };
  J j_ = J::Value;
  std::vector<char> nest_;     // '{' / '[' open
  bool key_ = false;           // the string read is a key
  bool esc_ = false;           // after a backslash
  int hex_left_ = 0;           // \uXXXX: hex digits still to come
  uint32_t hex_ = 0, high_ = 0;   // the code point so far; a high surrogate waiting for its low one
  std::string str_;            // the string read (up to the cap of where it goes)
  size_t str_len_ = 0;         // its length in bytes, also past the cap
  uint64_t hash_ = 0;          // FNV-1a over it
  size_t cap_ = 0;             // how much of it is kept
  std::string literal_;        // a number, true, false, null
  // the item read (depth 2)
  std::string item_, member_;
  ItemState st_;
  bool display_given_ = false, display_large_ = false, state_large_ = false;
  size_t state_len_ = 0;
  uint64_t state_hash_ = 0;
  std::string numeric_;
  void json_reset();
  void json_char(char c);
  void begin_string(bool key);
  void string_byte(char c);
  void string_utf8(uint32_t cp);
  void end_string();
  void end_value();            // a value at the current depth is complete
  void end_literal();
  void open(char c);
  void close(char c);

  size_t max_text_, max_picture_;
  ItemHandler on_item_;
  EventHandler on_event_;
  LargeHandler on_large_;
  int bad_events_ = 0;
  std::map<std::string, uint32_t> counts_;   // per picture item: the states that came (its address's version)
};

}  // namespace appoh
