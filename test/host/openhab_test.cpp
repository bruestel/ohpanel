/*
   Host test for the openHAB stream decoding: feeds a real capture of
   /rest/events/states (openHAB 5.1.4) through the SSE parser and the state
   decoder, cut into random pieces, and checks what comes out.

   Build and run: test/host/run.sh
*/

#include "Sse.h"
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
