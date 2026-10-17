/*
   Host test for the Nextion protocol parser.
*/

#include "nextion/Protocol.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using nextion::Message;

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                                   \
    }                                                                                 \
  } while (0)

std::string bytes(std::initializer_list<int> v) {
  std::string s;
  for (int b : v) s += static_cast<char>(b);
  return s;
}
const std::string END = bytes({0xFF, 0xFF, 0xFF});
}  // namespace

int main() {
  // A stream as the display design sends it: startup, ready, a touch, an event, a slider value,
  // a status byte, a get answer.
  const std::string json = R"({"page": "home", "component": "swipe", "value": "left"})";
  std::string stream = bytes({0, 0, 0}) + END + bytes({0x88}) + END + bytes({0x65, 0, 52, 0}) + END +
                       std::string("\x92nspanelevent", 13) + std::string(1, '\0') + json + std::string(1, '\0') + END +
                       std::string("\x91" "brightslider", 13) + std::string(1, '\0') + bytes({42, 0, 0, 0}) + END +
                       bytes({0x1A}) + END + bytes({0x71, 0x10, 0x27, 0, 0}) + END;

  std::mt19937 rng(1);
  for (size_t max_piece : {1, 3, 1000}) {
    std::vector<Message> got;
    nextion::Parser parser([&](const Message &m) { got.push_back(m); });
    std::uniform_int_distribution<size_t> piece(1, max_piece);
    for (size_t pos = 0; pos < stream.size();) {
      const size_t n = std::min(piece(rng), stream.size() - pos);
      parser.feed(reinterpret_cast<const uint8_t *>(stream.data() + pos), n);
      pos += n;
    }
    CHECK(got.size() == 7);
    if (got.size() != 7)
      continue;
    CHECK(got[0].kind == Message::Kind::Startup);
    CHECK(got[1].kind == Message::Kind::Ready);
    CHECK(got[2].kind == Message::Kind::Touch && got[2].page == 0 && got[2].component == 52 && !got[2].press);
    CHECK(got[3].kind == Message::Kind::Event && got[3].name == "nspanelevent" && got[3].text == json);
    CHECK(got[4].kind == Message::Kind::Value && got[4].name == "brightslider" && got[4].value == 42);
    CHECK(got[5].kind == Message::Kind::Error && got[5].code == 0x1A);
    CHECK(got[6].kind == Message::Kind::Number && got[6].value == 10000);
  }

  // a touch anywhere (sendxy=1): x 255 (a byte FF), y 300, released
  {
    std::vector<Message> got;
    nextion::Parser parser([&](const Message &m) { got.push_back(m); });
    const std::string point = bytes({0x67, 0x00, 0xFF, 0x01, 0x2C, 0x00}) + END;
    parser.feed(reinterpret_cast<const uint8_t *>(point.data()), point.size());
    CHECK(got.size() == 1 && got[0].kind == Message::Kind::Point && got[0].x == 255 && got[0].y == 300 &&
          !got[0].press);
  }

  // A frame without terminator is dropped once it exceeds the limit; parsing is back in step with
  // the next terminator.
  {
    std::vector<Message> got;
    nextion::Parser parser([&](const Message &m) { got.push_back(m); }, 64);
    std::string junk(200, 'x');
    parser.feed(reinterpret_cast<const uint8_t *>(junk.data()), junk.size());
    parser.feed(reinterpret_cast<const uint8_t *>(END.data()), END.size());
    const std::string ready = bytes({0x88}) + END;
    parser.feed(reinterpret_cast<const uint8_t *>(ready.data()), ready.size());
    CHECK(parser.overflows() >= 1);
    CHECK(!got.empty() && got.back().kind == Message::Kind::Ready);
  }

  CHECK(nextion::quote("say \"hi\"\\") == "say \\\"hi\\\"\\\\");

  // the answer to "sendme": the page shown
  {
    std::vector<Message> got;
    nextion::Parser parser([&](const Message &m) { got.push_back(m); });
    const std::string sendme = bytes({0x66, 0x0c}) + END;
    parser.feed(reinterpret_cast<const uint8_t *>(sendme.data()), sendme.size());
    CHECK(got.size() == 1 && got[0].kind == Message::Kind::Page && got[0].page == 12);
  }

  // the answer to "connect"
  {
    std::vector<Message> got;
    nextion::Parser parser([&](const Message &m) { got.push_back(m); });
    const std::string comok = "comok 1,30614-0,NX4832F035_011R,163,61488,D264B8204F0E1828,16777216" + END;
    parser.feed(reinterpret_cast<const uint8_t *>(comok.data()), comok.size());
    CHECK(got.size() == 1 && got[0].kind == Message::Kind::Connect);
    nextion::DeviceInfo info;
    CHECK(!got.empty() && nextion::parse_device_info(got[0].text, info));
    CHECK(info.model == "NX4832F035_011R" && info.firmware == "163" && info.mcu == "61488");
    CHECK(info.serial == "D264B8204F0E1828" && info.flash == 16777216 && info.touch);
    CHECK(!nextion::parse_device_info("1,2,3", info));
  }

  if (g_failures) {
    std::fprintf(stderr, "%d nextion check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("nextion: all checks passed\n");
  return 0;
}
