/*
   The display on the device.
*/

#include "appdisplay/Display.h"
#include "appdisplay/Icons.h"
#include "CoverLoad.h"

#include "appoh/OpenHab.h"

#include <atomic>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>

extern "C" {
// zlib-compressed (tools/gen_display_assets.py): inflated at the start, dropped once the engine has read them
extern const unsigned char display_manifest_json_z[], weather_icons_json_z[];
extern const uint32_t display_manifest_json_z_size, display_manifest_json_size;
extern const uint32_t weather_icons_json_z_size, weather_icons_json_size;
extern const char display_variant[];
}

namespace appdisplay {
namespace {

const char *const TAG = "display";

struct Job {
  enum class Kind { Message, State, Config, Status, Start, Button, Relay, Away, Options, Cover } kind;
  nextion::Message message;
  std::string text;
  PanelStatus status;
  int index = 0;
  bool on = false;
  int64_t at_ms = 0;   // a message: when it came (a swipe and a release are told apart by it)
  std::shared_ptr<Cover> cover;   // a cover loaded (text: its source)
};

QueueHandle_t g_queue = nullptr;
std::string g_manifest_json, g_weather_json;   // inflated by begin(), read by the task, then dropped

/* A zlib stream of the firmware inflated into `out`, `size` bytes -- the
   decompressor on the heap (about 11 KB, too much for the caller's stack;
   see icons_begin). */
bool inflate_asset(const unsigned char *z, size_t z_size, size_t size, std::string &out) {
  out.assign(size, '\0');
  auto *inflator = static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_DEFAULT));
  if (!inflator)
    return false;
  size_t in_len = z_size, got = size;
  tinfl_init(inflator);
  const tinfl_status status =
      tinfl_decompress(inflator, z, &in_len, reinterpret_cast<uint8_t *>(out.data()), reinterpret_cast<uint8_t *>(out.data()),
                       &got, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  heap_caps_free(inflator);
  if (status != TINFL_STATUS_DONE || got != size) {
    out.clear();
    return false;
  }
  return true;
}
std::mutex g_info_mx;
nextion::DeviceInfo g_info;
bool g_info_valid = false;
nextion::Display g_display;
Hooks g_hooks;

DesignCheck g_design;
/* "get design_ver" is pending: a number answers it. An "invalid variable"
   error may answer it too (a design without the variable) -- or
   belong to an earlier command the display only now rejects, which happens
   right after it restarted, while it is not on the page those commands were
   meant for. So an error only counts once no number followed for a while,
   and only when it came soon after the question: a later one belongs to a
   command drawn since (the question itself may have gone unanswered). */
bool g_design_asked = false;
bool g_design_error = false;
TickType_t g_design_asked_at = 0;
constexpr TickType_t kDesignErrorWait = pdMS_TO_TICKS(2000);
/* Asks the display who it is and which design it runs; both answers arrive
   as messages (Connect, then String or Error, see above). */
void ask_display() {
  {
    std::lock_guard<std::mutex> lock(g_info_mx);
    g_design_asked = true;
    g_design_error = false;
    g_design_asked_at = xTaskGetTickCount();
  }
  /* The version first: on the device (not in the simulator) the display
     swallows the command that follows "connect", so a harmless one after it
     goes to waste instead ("sendme": when not swallowed it is answered with the
     page number, which nobody waits for; an empty command was reported as
     invalid). */
  g_display.send("get design_ver");
  g_display.send("connect");
  g_display.send("sendme");
}

void design_answer(bool has, const std::string &version) {
  std::lock_guard<std::mutex> lock(g_info_mx);
  if (!g_design_asked)
    return;
  if (!has) {  // decided later, in design_timeout(), unless a string comes
    if (xTaskGetTickCount() - g_design_asked_at <= kDesignErrorWait)
      g_design_error = true;
    return;
  }
  g_design_asked = false;
  g_design.known = true;
  g_design.actual = has ? version : "";
  if (g_design.actual != g_design.expected)
    ESP_LOGW(TAG, "The display runs design \"%s\", this firmware was built for \"%s\" -- write the new display "
                  "firmware", g_design.actual.c_str(), g_design.expected.c_str());
}

/* An "invalid variable" with no number after it for a while means: the
   design has no version. (Not asked again: the next time the link comes up
   it is -- an extra "connect" while a page is drawn would cost a command.) */
void design_timeout() {
  bool finish = false;
  {
    std::lock_guard<std::mutex> lock(g_info_mx);
    const bool waited = xTaskGetTickCount() - g_design_asked_at > kDesignErrorWait;
    finish = g_design_asked && g_design_error && waited;
  }
  if (finish) {
    {
      std::lock_guard<std::mutex> lock(g_info_mx);
      g_design_error = false;
    }
    design_answer(true, "");
  }
}

int64_t g_page_sent_us = 0;  // when the last page change went out

/* ------------------------------------------------------------------ link --

   The link to the display, in one place. The engine only hears that it is up
   (link_up: the theme and the page drawn once) or down (link_down: nothing
   sent); messages reach it only while it is up.

     search    the display looked for at the rates it may run at (locate)
               every kSearchEveryUs, once the configuration has come (what
               the pages show, their theme first); found: identify. Still not found after
               kPowerCycleAfterUs: its power off and on, once.
     identify  who it is and which design it runs ("connect", "get
               design_ver"), before anything is drawn: the display drops
               commands that come right after "connect". Asked again every
               kAskAgainUs while it says nothing (it may let the first ask go
               by, busy right after it was found). Answered (or after
               kIdentifyUs): up.
     up        drawing. Down again when the display stops answering the flow
               control, does not answer when asked after kQuietUs without a
               word from it, or restarts (its "startup" / "ready").
               Identify unanswered: asked again after kAskLaterUs (and twice,
               three times that ...) between drawing, kAskLaterTimes at most.
     restart   it restarts (or was rewritten): its startup messages and boot
               page go by unheard; once it has been quiet for kSettledUs after
               them -- or after kRestartWaitUs at most --: search.
     upload    a display firmware is being written (tft_begin); afterwards
               restart.

   Our design runs at Config::baud from its first byte (Program.s "baud="), so
   there is no switching rates at runtime; another rate is only met with an
   older or another design. */
enum class Link { Search, Identify, Up, Restart, Upload };
const char *link_name(Link l) {
  switch (l) {
    case Link::Search: return "search";
    case Link::Identify: return "identify";
    case Link::Up: return "up";
    case Link::Restart: return "restart";
    case Link::Upload: return "upload";
  }
  return "?";
}
constexpr int64_t kSearchEveryUs = 2 * 1000 * 1000;
constexpr int64_t kIdentifyUs = 3 * 1000 * 1000;
constexpr int64_t kAskAgainUs = 1000 * 1000;
constexpr int64_t kAskLaterUs = 15 * 1000 * 1000;   // up without an answer from identify: asked again this often
constexpr int kAskLaterTimes = 4;                  // and this many times
constexpr int64_t kPowerCycleAfterUs = 20 * 1000 * 1000;
constexpr int64_t kQuietUs = 10 * 1000 * 1000;
constexpr int64_t kSettledUs = 500 * 1000;
constexpr int64_t kRestartWaitUs = 5 * 1000 * 1000;
constexpr int64_t kAfterUploadWaitUs = 30 * 1000 * 1000;   // a display just rewritten takes its time
constexpr int64_t kUploadStartUs = 15 * 1000 * 1000;       // an upload announced and not begun after this: none

struct LinkState {
  Link state = Link::Search;
  int64_t since_us = 0;        // in this state since
  int64_t wait_us = 0;         // restart: how long at most
  int64_t tried_us = 0;        // search: the last try
  int64_t heard_us = 0;        // the last message from the display
  bool restarted = false;      // restart: its startup messages came
  bool uploading = false;      // upload: the driver has begun it
  bool cycled = false;         // search: switched off and on already
  int asked = 0;               // up: asked again who it is, this often
};
LinkState g_link;

void link_enter(Link state, int64_t wait_us = 0) {
  if (state != g_link.state)
    ESP_LOGI(TAG, "display link: %s -> %s", link_name(g_link.state), link_name(state));
  g_link.state = state;
  g_link.since_us = esp_timer_get_time();
  g_link.wait_us = wait_us;
  g_link.tried_us = 0;
  g_link.restarted = false;
  g_link.uploading = false;
  g_link.asked = 0;
}

void link_down(Engine &engine, Link next, int64_t wait_us = 0) {
  if (g_link.state == Link::Up)
    engine.link_down();
  link_enter(next, wait_us);
}

/* A message from the display: true if the engine is to have it. */
bool link_message(Engine &engine, const nextion::Message &m) {
  g_link.heard_us = esp_timer_get_time();
  using Kind = nextion::Message::Kind;
  if (m.kind == Kind::Startup || m.kind == Kind::Ready) {
    if (g_link.state == Link::Upload)
      return false;
    if (g_link.state != Link::Restart)
      link_down(engine, Link::Restart, kRestartWaitUs);
    g_link.restarted = true;
    return false;
  }
  return g_link.state == Link::Up;
}

void link_go_up(Engine &engine) {
  link_enter(Link::Up);
  g_link.cycled = false;
  g_link.heard_us = esp_timer_get_time();
  engine.link_up();
}

/* One step of the link; called on every turn of the display task. */
void link_step(Engine &engine) {
  const int64_t now = esp_timer_get_time();
  if (g_display.uploading()) {
    if (g_link.state != Link::Upload)
      link_down(engine, Link::Upload);
    g_link.uploading = true;
    return;
  }
  switch (g_link.state) {
    case Link::Upload:
      // over: the display restarts with what was written (or what it had) -- or it never began (tft_begin
      // announces it first, then the driver looks for the display at every rate)
      if (g_link.uploading || now - g_link.since_us > kUploadStartUs)
        link_enter(Link::Restart, kAfterUploadWaitUs);
      break;
    case Link::Restart:
      if ((g_link.restarted && now - g_link.heard_us > kSettledUs) || now - g_link.since_us > g_link.wait_us)
        link_enter(Link::Search);
      break;
    case Link::Search: {
      // not before the configuration has come: the theme and the background the pages show are in it
      if (!engine.configured() || (g_link.tried_us && now - g_link.tried_us < kSearchEveryUs))
        break;
      g_link.tried_us = now;
      if (const int baud = g_display.locate()) {
        link_enter(Link::Identify);
        ESP_LOGI(TAG, "display found at %d baud", baud);
        ask_display();
        g_link.tried_us = now;
        break;
      }
      /* Not found for a while: stuck (an upload broken off leaves it waiting for the rest) -- its power
         off and on, once (a restart of the ESP alone does not reach it). */
      if (!g_link.cycled && now - g_link.since_us > kPowerCycleAfterUs && g_hooks.power_cycle) {
        g_link.cycled = true;
        ESP_LOGW(TAG, "no answer from the display -- switching it off and on");
        g_hooks.power_cycle();
        link_enter(Link::Restart, kRestartWaitUs);
      }
      break;
    }
    case Link::Identify:
      if (now - g_link.tried_us > kAskAgainUs && now - g_link.since_us < kIdentifyUs) {
        g_link.tried_us = now;   // nothing yet (the Connect answer moves on): once more
        ask_display();
      } else if (now - g_link.since_us > kIdentifyUs) {
        ESP_LOGW(TAG, "the display does not say who it is -- drawing anyway");
        link_go_up(engine);
      }
      break;
    case Link::Up: {
      bool identified;
      {
        std::lock_guard<std::mutex> lock(g_info_mx);
        identified = g_info_valid && g_design.known;
      }
      if (g_display.lost()) {
        ESP_LOGW(TAG, "the display stopped answering -- looking for it again");
        link_down(engine, Link::Search);
      } else if (now - g_link.heard_us > kQuietUs) {
        // quiet for a while: still there? (one that restarted unheard -- a brownout -- says nothing)
        g_link.heard_us = now;
        if (!g_display.ping() && !g_display.ping()) {
          ESP_LOGW(TAG, "the display does not answer -- looking for it again");
          link_down(engine, Link::Search);
        }
      } else if (!identified && g_link.asked < kAskLaterTimes && now - g_link.since_us > (g_link.asked + 1) * kAskLaterUs &&
                 !engine.pending()) {
        /* identify went by without an answer (the display busy right after it was found): who it is and its
           design asked again, between drawing -- the status page and the design check need them */
        g_link.asked++;
        ESP_LOGI(TAG, "asking the display again who it is (%d of %d)", g_link.asked, kAskLaterTimes);
        ask_display();
      }
      break;
    }
  }
}

class UartSink : public Sink {
 public:
  void send(const std::string &command) override {
    if (command.rfind("page ", 0) == 0)
      g_page_sent_us = esp_timer_get_time();
    g_display.send(command);
  }
};

std::atomic<bool> g_states_lost{false};

void post(Job *job) {
  /* Never blocks the caller (the openHAB stream, the UART reader). A state
     that does not fit is not lost for good: the page is drawn again from the
     states once the queue has emptied (a big configuration's snapshot after a
     reconnect can outrun the queue). */
  if (!g_queue || xQueueSend(g_queue, &job, 0) != pdPASS) {
    if (job->kind == Job::Kind::State)
      g_states_lost = true;
    else
      ESP_LOGW(TAG, "queue full, dropping a display job");
    delete job;
  }
}

void task(void *) {
  UartSink sink;
  Callbacks cb;
  cb.state = [](const std::string &item, appoh::ItemState &out) { return appoh::state(item, out); };
  cb.command = [](const std::string &item, const std::string &command) { appoh::send_command(item, command); };
  cb.relay = [](int index, bool on) {
    if (g_hooks.relay) g_hooks.relay(index, on);
  };
  cb.restart = [] {
    if (g_hooks.restart) g_hooks.restart();
  };
  cb.options = [](const std::string &item, std::vector<appoh::Option> &out, bool refresh) {
    return appoh::options(item, out, refresh);
  };
  cb.expect = [](const std::string &item, const std::string &state) { appoh::expect(item, state); };
  cb.limits = [](const std::string &item, appoh::Limits &out) { return appoh::limits(item, out); };
  cb.members = [](const std::string &group, std::vector<appoh::Member> &out, bool refresh) {
    return appoh::members(group, out, refresh);
  };
  cb.metadata = [](const std::string &item, std::map<std::string, appoh::Metadata> &out, bool refresh) {
    return appoh::metadata(item, out, refresh);
  };
  // an item's options have come (from their own task): pages listing them redraw
  appoh::set_options_listener([](const std::string &item) { post(new Job{Job::Kind::Options, {}, item, {}, 0, false}); });
  // a cover picture: loaded in a task of its own (CoverLoad.cpp), drawn by the engine once it has come
  cb.cover = [](const std::string &source, int size, int scale, int radius, Reveal reveal) {
    cover_load(source, size, scale, radius, reveal);
  };
  cover_begin([](const std::string &source, std::shared_ptr<Cover> cover) {
    Job *job = new Job{Job::Kind::Cover, {}, source, {}, 0, false};
    job->cover = std::move(cover);
    post(job);
  });
  cb.remember = [](const std::string &key, int value) {
    if (g_hooks.remember) g_hooks.remember(key, value);
  };
  cb.system = [] {
    SystemInfo i = g_hooks.system ? g_hooks.system() : SystemInfo{};
    nextion::DeviceInfo d;
    if (display_info(d)) {
      i.display_model = d.model;
      i.display_firmware = d.firmware;
    }
    const DesignCheck c = design_check();
    i.display_outdated = c.known && !c.current();
    i.display_design = c.known ? c.actual : "";
    return i;
  };
  Engine engine(sink, cb);
  engine.link_down();   // nothing drawn before the display is found and has said who it is
  if (!engine.load_design(g_manifest_json.c_str(), g_weather_json.c_str()))
    ESP_LOGE(TAG, "display manifest or weather icon sets do not parse");
  std::string().swap(g_manifest_json);   // read: not needed any more
  std::string().swap(g_weather_json);
  link_enter(Link::Search);

  for (;;) {
    Job *job = nullptr;
    design_timeout();
    link_step(engine);
    // animated icons want their frames on time, a release waits briefly for a swipe: a short wait then
    if (xQueueReceive(g_queue, &job, pdMS_TO_TICKS(engine.pending() ? 20 : engine.animating() ? 50 : 1000)) ==
            pdPASS &&
        job) {
      // events timed when they came, not when their turn comes: a job before them (a page drawn) may take
      // a while, and a release queued behind a swipe would count as late -- a tap
      engine.clock(job->at_ms ? job->at_ms : esp_timer_get_time() / 1000);
      switch (job->kind) {
        case Job::Kind::Message:
          if (job->message.kind == nextion::Message::Kind::Number) {
            // the version's 8 hex digits, stored as a signed 32-bit integer
            char hex[9];
            snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(static_cast<uint32_t>(job->message.value)));
            design_answer(true, hex);
          } else if (job->message.kind == nextion::Message::Kind::Error && job->message.code == 0x1A) {
            design_answer(false, "");  // invalid variable: a design without design_ver
          } else if (job->message.kind == nextion::Message::Kind::Connect) {
            nextion::DeviceInfo d;
            if (nextion::parse_device_info(job->message.text, d)) {
              ESP_LOGI(TAG, "Display %s, firmware %s, serial %s", d.model.c_str(), d.firmware.c_str(),
                       d.serial.c_str());
              {
                std::lock_guard<std::mutex> lock(g_info_mx);
                g_info = d;
                g_info_valid = true;
              }
              if (g_link.state == Link::Identify)
                link_go_up(engine);   // the commands after "connect" went to waste meanwhile: drawing now
            }
          }
          if (!link_message(engine, job->message))
            break;
          {
            // how long a page change takes: the display loading it, the page built (normally
            // 120 to 650 ms; past 800 ms the page shows its busy indicator)
            const int64_t t0 = esp_timer_get_time();
            const bool page = job->message.kind == nextion::Message::Kind::Event &&
                              job->message.text.find("pagechange") != std::string::npos;
            engine.on_message(job->message);
            if (page && g_page_sent_us) {
              const long long load = (t0 - g_page_sent_us) / 1000, build = (esp_timer_get_time() - t0) / 1000;
              if (load + build > 1000)
                ESP_LOGW(TAG, "slow page change: display %lld ms, built in %lld ms", load, build);
              else
                ESP_LOGD(TAG, "page change: display %lld ms, built in %lld ms", load, build);
              g_page_sent_us = 0;
            }
          }
          break;
        case Job::Kind::State: engine.on_state(job->text); break;
        case Job::Kind::Config: engine.set_config(job->text); break;
        case Job::Kind::Status: engine.set_status(job->status); break;
        case Job::Kind::Start: engine.start(); break;
        case Job::Kind::Button: engine.hardware_button(job->index); break;
        case Job::Kind::Relay: engine.set_relay(job->index, job->on); break;
        case Job::Kind::Away: link_down(engine, Link::Upload); break;   // an upload begins
        case Job::Kind::Options: engine.on_options(job->text); break;
        case Job::Kind::Cover: engine.on_cover(job->text, job->cover); break;
      }
      delete job;
    }
    if (uxQueueMessagesWaiting(g_queue) == 0 && g_states_lost.exchange(false)) {
      ESP_LOGW(TAG, "states came faster than the display queue took them -- the page drawn again");
      engine.redraw_states();
    }
    engine.tick(time(nullptr));
    engine.animate(esp_timer_get_time() / 1000);
    engine.idle(esp_timer_get_time() / 1000);
  }
}

}  // namespace

bool begin(const nextion::Config &uart, Hooks hooks) {
  g_hooks = std::move(hooks);
  if (!inflate_asset(display_manifest_json_z, display_manifest_json_z_size, display_manifest_json_size, g_manifest_json) ||
      !inflate_asset(weather_icons_json_z, weather_icons_json_z_size, weather_icons_json_size, g_weather_json))
    ESP_LOGE(TAG, "the embedded display manifest or weather icon sets are damaged");
  if (cJSON *m = cJSON_Parse(g_manifest_json.c_str())) {
    const cJSON *v = cJSON_GetObjectItem(m, "design");
    g_design.expected = cJSON_IsString(v) ? v->valuestring : "";
    cJSON_Delete(m);
  }
  icons_begin();
  // the openHAB snapshot after a (re)connect: a state for every item at once
  g_queue = xQueueCreate(128, sizeof(Job *));
  if (!g_queue)
    return false;
  /* The UART first: the task talks to the display as soon as it runs. */
  if (!g_display.begin(uart, [](const nextion::Message &m) {
        post(new Job{Job::Kind::Message, m, {}, {}, 0, false, esp_timer_get_time() / 1000});
      }))
    return false;
  /* 16 KB: the info page and its network page build their rows (SystemInfo, its address lists) on the stack;
     8 KB left 512 bytes before them and overflowed with them, 10 KB left 2 KB. Link-time optimisation inlines more
     into each frame: 14 KB left under 3 KB. Room for what pages come (diagnostics::check_stacks warns below 1 KB). */
  return xTaskCreate(task, "display", 16384, nullptr, 4, nullptr) == pdPASS;
}

void config_changed(const std::string &json) { post(new Job{Job::Kind::Config, {}, json, {}, 0, false}); }
void state_changed(const std::string &item) { post(new Job{Job::Kind::State, {}, item, {}, 0, false}); }
void status(const PanelStatus &s) { post(new Job{Job::Kind::Status, {}, {}, s, 0, false}); }
void start() { post(new Job{Job::Kind::Start, {}, {}, {}, 0, false}); }
void hardware_button(int index) { post(new Job{Job::Kind::Button, {}, {}, {}, index, false}); }
void relay_state(int index, bool on) { post(new Job{Job::Kind::Relay, {}, {}, {}, index, on}); }

DesignCheck design_check() {
  std::lock_guard<std::mutex> lock(g_info_mx);
  return g_design;
}

int link_baud() { return g_display.baud(); }

const char *variant() { return display_variant; }

bool display_info(nextion::DeviceInfo &out) {
  std::lock_guard<std::mutex> lock(g_info_mx);
  out = g_info;
  return g_info_valid;
}

// ------------------------------------------------------------------- tft --

void log_commands(bool on) { g_display.log_commands(on); }
bool logging_commands() { return g_display.logging_commands(); }

void send_raw(const std::string &command) {
  ESP_LOGI(TAG, "> %s (from the web interface)", command.c_str());
  g_display.send(command);
}

// the display restarts after an upload, with its boot page: nothing of ours to draw on
static void display_away() { post(new Job{Job::Kind::Away, {}, {}, {}, 0, false}); }

/* The display switched off and on first, every time: an upload then meets it as it starts, not in what it
   was told since -- protocol reparse mode, which the firmware before switches on (Sonoff's, Tasmota's
   driver) and a restart of the ESP alone does not end, a rate changed, an upload broken off. A design
   that enters that mode by itself is asked to leave it (nextion::Display::upload_begin). */
bool tft_begin(size_t size, int baud, std::string *error) {
  display_away();
  if (g_hooks.power_cycle) {
    ESP_LOGI(TAG, "switching the display off and on before the upload");
    g_hooks.power_cycle();
  }
  if (g_display.upload_begin(size, baud, error))
    return true;
  // no answer at any rate: once more after switching it off and on
  if (!g_hooks.power_cycle)
    return false;
  ESP_LOGW(TAG, "the display does not take the upload -- switching it off and on, then again");
  g_hooks.power_cycle();
  return g_display.upload_begin(size, baud, error);
}
bool tft_write(const uint8_t *data, size_t len, std::string *error) { return g_display.upload_write(data, len, error); }
bool tft_end(std::string *error) { return g_display.upload_end(error); }
void tft_abort() { g_display.upload_abort(); }

}  // namespace appdisplay
