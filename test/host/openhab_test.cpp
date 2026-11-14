/*
   Host test for the openHAB stream decoding: feeds a real capture of
   /rest/events/states (openHAB 5.1.4) through the SSE parser and the state
   decoder, cut into random pieces, and checks what comes out.

   Build and run: test/host/run.sh
*/

#include "Sse.h"
#include "StateStream.h"
#include "States.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

using namespace appoh;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

struct Result {
  int ready = 0, alive = 0, state_events = 0, bad_states = 0;
  std::string conn_id;
  std::map<std::string, ItemState> states;
};

Result run(const std::string &stream, std::mt19937 &rng, size_t max_piece) {
  Result r;
  SseParser parser(128 * 1024, [&](const std::string &event, const std::string &data) {
    if (event == "ready") {
      r.ready++;
      r.conn_id = data;
    } else if (event == "alive") {
      r.alive++;
    } else if (event.empty()) {
      r.state_events++;
      if (!parse_states(data, [&](const std::string &item, const ItemState &s) { r.states[item] = s; }))
        r.bad_states++;
    }
  });
  std::uniform_int_distribution<size_t> piece(1, max_piece);
  for (size_t pos = 0; pos < stream.size();) {
    const size_t n = std::min(piece(rng), stream.size() - pos);
    CHECK(parser.feed(stream.data() + pos, n));
    pos += n;
  }
  return r;
}

std::string crlf(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\n')
      out += '\r';
    out += c;
  }
  return out;
}

/* The stream read item by item (StateStream), cut into random pieces. */
Result run_stream(const std::string &stream, std::mt19937 &rng, size_t max_piece, size_t max_text = 64 * 1024,
                  size_t max_picture = 16 * 1024, std::vector<std::string> *large = nullptr, int *bad = nullptr) {
  Result r;
  StateStream parser(max_text, max_picture,
      [&](const std::string &item, const ItemState &s) { r.states[item] = s; },
      [&](const std::string &event, const std::string &data) {
        if (event == "ready") {
          r.ready++;
          r.conn_id = data;
        } else if (event == "alive") {
          r.alive++;
        }
      },
      [&](const std::string &item, size_t bytes, bool as_address) {
        if (large)
          large->push_back(item + (as_address ? " address " : " left out ") + std::to_string(bytes));
      });
  std::uniform_int_distribution<size_t> piece(1, max_piece);
  for (size_t pos = 0; pos < stream.size();) {
    const size_t n = std::min(piece(rng), stream.size() - pos);
    parser.feed(stream.data() + pos, n);
    pos += n;
  }
  if (bad)
    *bad = parser.bad_events();
  return r;
}

bool same(const ItemState &a, const ItemState &b) {
  return a.state == b.state && a.display == b.display && a.type == b.type && a.unit == b.unit &&
         a.has_numeric == b.has_numeric && (!a.has_numeric || a.numeric == b.numeric);
}

std::string b64(const std::string &bytes) {
  static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = static_cast<uint8_t>(bytes[i]) << 16 | static_cast<uint8_t>(bytes[i + 1]) << 8 |
                       static_cast<uint8_t>(bytes[i + 2]);
    out += abc[v >> 18 & 63]; out += abc[v >> 12 & 63]; out += abc[v >> 6 & 63]; out += abc[v & 63];
  }
  if (i < bytes.size()) {
    uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) v |= static_cast<uint8_t>(bytes[i + 1]) << 8;
    out += abc[v >> 18 & 63]; out += abc[v >> 12 & 63];
    out += i + 1 < bytes.size() ? abc[v >> 6 & 63] : '=';
    out += '=';
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  std::ifstream f(argc > 1 ? argv[1] : "test/host/data/openhab-5.1.4-states.sse", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string capture = ss.str();
  CHECK(!capture.empty());

  std::mt19937 rng(42);
  Result reference;
  for (const std::string &stream : {capture, crlf(capture)}) {
    for (size_t max_piece : {1, 7, 100, 1024, 1 << 20}) {
      for (int round = 0; round < 20; round++) {
        Result r = run(stream, rng, max_piece);
        CHECK(r.ready == 1);
        CHECK(r.conn_id.size() == 36);  // a UUID
        CHECK(r.alive >= 2);
        CHECK(r.state_events >= 1);
        CHECK(r.bad_states == 0);
        if (reference.states.empty())
          reference = r;
        CHECK(r.states.size() == reference.states.size());
        CHECK(r.state_events == reference.state_events);
      }
    }
  }

  /* Decoding, against values the capture is known to contain. */
  const auto &st = reference.states;
  CHECK(st.count("DoesNotExist_OHpanel") == 0);  // unknown items are skipped by openHAB
  if (st.count("Panel_LivingRoom__DimmedBrightness")) {
    const ItemState &d = st.at("Panel_LivingRoom__DimmedBrightness");
    CHECK(d.type == "Percent");
    CHECK(d.display == "80 %");
    CHECK(d.has_numeric);
  } else {
    CHECK(!"dimmer missing from capture");
  }
  for (const auto &[name, s] : st) {
    CHECK(!s.type.empty());
    CHECK(!s.display.empty() || s.state.empty() || s.type == "String");
    if (s.type == "Quantity")
      CHECK(!s.unit.empty() && s.has_numeric);
    if (s.type == "UnDef")
      CHECK(s.undefined() && !s.has_numeric);
  }

  /* An openHAB without numericState (older ones): the number read from a number's state, the unit from its display
     text; a text is no number (forum #124). */
  {
    std::map<std::string, ItemState> old;
    CHECK(parse_states(R"({"T": {"state": "8.6 °C", "displayState": "8.6 °C", "type": "Quantity"},
                           "N": {"state": "20.1", "type": "Decimal"}, "P": {"state": "45", "type": "Percent"},
                           "S": {"state": "12 apples", "type": "String"}, "U": {"state": "NULL", "type": "UnDef"},
                           "X": {"state": "-", "type": "Decimal"}, "K": {"state": "2700 K", "type": "Quantity"},
                           "W": {"state": "1.5E3 W", "type": "Quantity", "unit": "kW"},
                           "H": {"state": "0x10", "type": "Decimal"}, "A": {"state": "12abc", "type": "Decimal"},
                           "L": {"state": " 5", "type": "Decimal"}})",
                       [&](const std::string &item, const ItemState &s) { old[item] = s; }));
    CHECK(old["T"].has_numeric && old["T"].numeric == 8.6 && old["T"].unit == "°C");
    CHECK(old["N"].unit.empty() && old["P"].unit.empty());   // a quantity's only
    CHECK(old["N"].has_numeric && old["N"].numeric == 20.1);
    CHECK(old["P"].has_numeric && old["P"].numeric == 45);
    CHECK(!old["S"].has_numeric && !old["U"].has_numeric && !old["X"].has_numeric);
    CHECK(old["K"].unit == "K" && old["K"].numeric == 2700);       // a colour temperature in Kelvin
    CHECK(old["W"].unit == "W" && old["W"].numeric == 1500);       // the unit of the number read, not another
    CHECK(!old["H"].has_numeric && !old["A"].has_numeric && !old["L"].has_numeric);   // no hex, no text after, no space
  }


  /* The stream read item by item gives what the whole line parsed gave, however it is cut. */
  for (const std::string &stream : {capture, crlf(capture)}) {
    for (size_t max_piece : {1, 3, 100, 1024, 1 << 20}) {
      for (int round = 0; round < 5; round++) {
        int bad = -1;
        Result r = run_stream(stream, rng, max_piece, 64 * 1024, 16 * 1024, nullptr, &bad);
        CHECK(bad == 0);
        CHECK(r.ready == 1 && r.conn_id == reference.conn_id);
        CHECK(r.alive == reference.alive);
        CHECK(r.states.size() == reference.states.size());
        for (const auto &[name, s] : reference.states)
          CHECK(r.states.count(name) && same(r.states.at(name), s));
      }
    }
  }

  /* A picture too large to keep: its address on openHAB instead (the same picture: the same address), the stream
     goes on; text that large is left out; a small picture stays as it is (forum: a cover of 100 KB dropped the
     stream, and the snapshot after reconnecting again). */
  {
    std::string big;
    for (int i = 0; i < 300 * 1024; i++)
      big += static_cast<char>(i * 7 + i / 13);
    const std::string pic = "data:image/jpeg;base64," + b64(big), small_pic = "data:image/png;base64," + b64("tiny");
    const std::string text(70 * 1024, 'x');
    const std::string snapshot = "event: ready\ndata: 0123\n\n"
        "data: {\"Cover\":{\"state\":\"" + pic + "\",\"type\":\"Raw\"},"
        "\"Small\":{\"state\":\"" + small_pic + "\",\"type\":\"Raw\"},"
        "\"Text\":{\"type\":\"String\",\"state\":\"" + text + "\"},"
        "\"Temp\":{\"state\":\"21.5 \\u00b0C\",\"numericState\":21.5,\"unit\":\"\\u00b0C\",\"type\":\"Quantity\","
        "\"extra\":[1,{\"a\":[]},\"s\",null,true]},"
        "\"Emoji\":{\"state\":\"\\ud83d\\ude00 \\\"q\\\" \\\\ / \\n\",\"type\":\"String\"}}\n\n"
        "event: alive\ndata: {\"interval\":10}\n\n"
        "data: {\"Cover\":{\"state\":\"" + pic + "\",\"type\":\"Raw\"}}\n\n";
    for (size_t max_piece : {1, 50, 4096}) {
      std::vector<std::string> large;
      int bad = -1;
      Result r = run_stream(snapshot, rng, max_piece, 64 * 1024, 16 * 1024, &large, &bad);
      CHECK(bad == 0);
      CHECK(r.ready == 1 && r.alive == 1);
      CHECK(r.states.count("Cover") && r.states["Cover"].state.rfind("/rest/items/Cover/state?v=", 0) == 0);
      CHECK(r.states["Cover"].state.size() == std::string("/rest/items/Cover/state?v=").size() + 16);
      CHECK(r.states["Cover"].display == r.states["Cover"].state);
      CHECK(r.states["Small"].state == small_pic);
      CHECK(r.states.count("Text") == 0);
      CHECK(r.states["Temp"].state == "21.5 \u00b0C" && r.states["Temp"].unit == "\u00b0C");
      CHECK(r.states["Temp"].has_numeric && r.states["Temp"].numeric == 21.5);
      CHECK(r.states["Emoji"].state == "\xF0\x9F\x98\x80 \"q\" \\ / \n");
      CHECK(large.size() == 3);   // the cover twice, the text once
      if (large.size() == 3) {
        CHECK(large[0].rfind("Cover address ", 0) == 0);
        CHECK(large[1] == "Text left out " + std::to_string(text.size()));
      }
    }
    // another picture: another address
    std::string other = big;
    other[1000] ^= 1;
    Result a = run_stream("data: {\"C\":{\"state\":\"data:image/jpeg;base64," + b64(big) + "\",\"type\":\"Raw\"}}\n\n", rng, 999);
    Result b = run_stream("data: {\"C\":{\"state\":\"data:image/jpeg;base64," + b64(other) + "\",\"type\":\"Raw\"}}\n\n", rng, 999);
    CHECK(!a.states["C"].state.empty() && a.states["C"].state != b.states["C"].state);
    // broken JSON: that event is lost, the next one comes
    int bad = -1;
    Result c = run_stream("data: {\"A\":{\"state\":\"1\"]]\n\ndata: {\"B\":{\"state\":\"2\",\"type\":\"Decimal\"}}\n\n",
                          rng, 3, 64 * 1024, 16 * 1024, nullptr, &bad);
    CHECK(bad == 1 && c.states.count("B") && c.states["B"].numeric == 2);
    // as openHAB 5 sends a picture: its size only; each state a new address, the same text too
    std::vector<std::string> large;
    const std::string raw = "data: {\"Cam\":{\"state\":\"raw type (image/jpeg): 219955 bytes\",\"type\":\"Raw\"}}\n\n";
    StateStream p5(64 * 1024, 16 * 1024, [&](const std::string &item, const ItemState &s) { large.push_back(item + " " + s.state); },
                   {}, [&](const std::string &item, size_t bytes, bool as_address) {
                     large.push_back(item + (as_address ? " address " : " out ") + std::to_string(bytes));
                   });
    p5.feed(raw.data(), raw.size());
    p5.feed(raw.data(), raw.size());
    CHECK(large.size() == 4);
    if (large.size() == 4) {
      CHECK(large[0] == "Cam address 219955");
      CHECK(large[1].rfind("Cam /rest/items/Cam/state?v=", 0) == 0);
      CHECK(large[3].rfind("Cam /rest/items/Cam/state?v=", 0) == 0 && large[3] != large[1]);
    }
  }

  std::printf("%zu items decoded from %d state events (%d keepalives)\n", st.size(), reference.state_events,
              reference.alive);
  for (const auto &[name, s] : st)
    std::printf("  %-45s %-9s %s\n", name.c_str(), s.type.c_str(), s.display.c_str());

  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
