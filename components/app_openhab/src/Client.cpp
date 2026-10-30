/*
   openHAB REST client: the state stream and the command sender.

   Two tasks, two connections. The stream task holds the long-lived
   /rest/events/states connection and does nothing but read it; the worker task
   sends everything else (the subscription list, commands) on short requests of
   its own. A command therefore never waits behind a stream that is busy or
   stalled, and the stream never stops reading to wait for a POST.
*/

#include "appoh/OpenHab.h"
#include "Http.h"
#include "Sse.h"
#include "States.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>

namespace appoh {
namespace {

const char *const TAG = "appoh";

/* Connecting and the response headers get this long. */
constexpr int kConnectTimeoutMs = 10000;

/* After the headers the stream is read in short slices. esp_http_client_read()
   keeps reading until the requested length has arrived or the timeout expires,
   so with a long timeout a 100-byte state change would sit in the buffer until
   enough later traffic filled it. A short slice hands every event over within
   this time; silence is judged separately, below. */
constexpr int kReadSliceMs = 100;

/* openHAB sends "event: alive" every 10 s. Three missed keepalives mean the
   link is dead even if TCP has not noticed yet -- which, without traffic of our
   own, it would take minutes to. */
constexpr int64_t kSilenceUs = 30 * 1000000LL;

/* The snapshot after subscribing is a single data line holding every item;
   150 items come to 10-20 KB. Anything far beyond is not a state event. Large
   strings land in PSRAM by themselves (CONFIG_SPIRAM_USE_MALLOC). */
constexpr size_t kMaxEventBytes = 128 * 1024;

constexpr int64_t kBackoffMinUs = 1 * 1000000LL;
constexpr int64_t kBackoffMaxUs = 30 * 1000000LL;

constexpr int kRequestTimeoutMs = 5000;
constexpr int kJobQueueLen = 16;

struct Job {
  enum class Kind { Subscribe, Command, State } kind;
  std::string item;
  std::string value;
  bool secret = false;   // a code: not in the log, not as the item's expected state
};

/* Guards everything below it. */
SemaphoreHandle_t g_mux = nullptr;
std::vector<std::string> g_items;
std::vector<std::string> g_groups;   // their members subscribed too
std::vector<std::string> g_sent;     // what was last subscribed: the items and the groups' members (sorted)
std::string g_conn_id;  // the open stream's connection id; empty while there is none
bool g_subscribed = false;
std::map<std::string, ItemState> g_cache;
/* What a command will most likely make of an item's state, set in the cache
   at once (the page shows it without waiting for openHAB's echo), with the
   state it had: openHAB reports nothing when the command changes nothing --
   a device that did not take it -- so without a state from openHAB within
   kPredictUs the old one comes back. */
struct Predicted {
  ItemState before;
  int64_t until = 0;
};
std::map<std::string, Predicted> g_predicted;
constexpr int64_t kPredictUs = 5 * 1000000LL;
constexpr int64_t kPredictTextUs = 15 * 1000000LL;   // a stream to start (a URI) takes longer
esp_timer_handle_t g_predict_timer = nullptr;
uint32_t g_reconnects = 0;
std::string g_last_error;

StateCallback g_on_state;
std::atomic<bool> g_net_up{false};
/* Bumped to make the stream task drop its connection and start over. */
std::atomic<uint32_t> g_generation{0};
TaskHandle_t g_stream_task = nullptr;
QueueHandle_t g_jobs = nullptr;

class Lock {
 public:
  Lock() { xSemaphoreTake(g_mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mux); }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

void set_error(const std::string &e) {
  ESP_LOGW(TAG, "%s", e.c_str());
  Lock l;
  g_last_error = e;
}

void wake_stream() {
  if (g_stream_task)
    xTaskNotifyGive(g_stream_task);
}

bool enqueue(Job *job) {
  if (!g_jobs || xQueueSend(g_jobs, &job, 0) != pdPASS) {
    delete job;
    return false;
  }
  return true;
}

esp_http_client_handle_t make_client(const appcfg::OpenHab &cfg, const std::string &url, int timeout_ms,
                                     bool keep_session = false) {
  esp_http_client_config_t hc = {};
  hc.url = url.c_str();
  hc.timeout_ms = timeout_ms;
  hc.buffer_size = 2048;
  hc.keep_alive_enable = true;  // TCP keep-alive: a kept connection notices a dead peer
  apply_tls(hc, cfg);
  hc.save_client_session = keep_session;  // a reconnect resumes the TLS session: no full handshake
  esp_http_client_handle_t client = esp_http_client_init(&hc);
  if (client && !cfg.token.empty()) {
    const std::string auth = "Bearer " + cfg.token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  }
  return client;
}

/* The worker's connection, kept open between requests: over https a new
   connection costs a TLS handshake of a few seconds on this chip, so commands
   and subscriptions reuse one (HTTP keep-alive). Made anew when the settings
   change or a request fails. Worker task only. */
esp_http_client_handle_t g_keep = nullptr;
appcfg::OpenHab g_keep_cfg;  // what g_keep was made with; holds the CA it points into
int64_t g_keep_used_at = 0;
int64_t g_keep_made_at = 0;   // its full handshake: the session every reconnect resumes dates from it
/* openHAB's server closes a connection idle for 30 s; one older than this is
   closed before use, not tried (and logged as failing) first. The handle stays:
   it keeps the TLS session, and the next request resumes it in a fraction of
   the full handshake (the first switch after a pause waited seconds). */
constexpr int64_t kKeepIdleUs = 25 * 1000000LL;

bool same_settings(const appcfg::OpenHab &a, const appcfg::OpenHab &b) {
  return a.url == b.url && a.token == b.token && a.tls == b.tls && a.ca == b.ca && a.check_name == b.check_name;
}

void drop_keep() {
  if (g_keep)
    esp_http_client_cleanup(g_keep);
  g_keep = nullptr;
}

void close_keep() {
  if (g_keep)
    esp_http_client_close(g_keep);
}

/* A short POST (or PUT) from the worker task; returns the HTTP status, or 0
   if the request did not get through at all. */
int post(const appcfg::OpenHab &cfg, const std::string &path, const char *content_type, const std::string &body,
         esp_http_client_method_t method = HTTP_METHOD_POST) {
  const std::string url = cfg.url + path;
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
    // a kept connection may have been closed by the server: once more on a new one
    if (g_keep && !same_settings(cfg, g_keep_cfg))
      drop_keep();
    else if (attempt > 0 || esp_timer_get_time() - g_keep_used_at > kKeepIdleUs)
      close_keep();
    if (!g_keep) {
      g_keep_cfg = cfg;
      g_keep = make_client(g_keep_cfg, url, kRequestTimeoutMs, true);
      if (!g_keep)
        return 0;
      g_keep_made_at = esp_timer_get_time();
    } else {
      esp_http_client_set_url(g_keep, url.c_str());
    }
    esp_http_client_set_method(g_keep, method);
    esp_http_client_set_header(g_keep, "Content-Type", content_type);
    esp_http_client_set_header(g_keep, "X-OpenHAB-Source", "ohpanel");
    esp_http_client_set_post_field(g_keep, body.data(), static_cast<int>(body.size()));
    err = esp_http_client_perform(g_keep);
    g_keep_used_at = esp_timer_get_time();
  }
  const int status = err == ESP_OK ? esp_http_client_get_status_code(g_keep) : 0;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s %s failed: %s", method == HTTP_METHOD_PUT ? "PUT" : "POST", path.c_str(),
             esp_err_to_name(err));
    close_keep();
  }
  return status;
}

// ------------------------------------------------------------ state events ----

/* Stores every state of an event in the cache and hands it to the callback. */
void handle_states(const std::string &data) {
  const bool ok = parse_states(data, [](const std::string &item, const ItemState &s) {
    {
      Lock l;
      g_cache[item] = s;
      g_predicted.erase(item);   // openHAB's word counts
    }
    /* Outside the lock: the callback may well ask for other states. */
    if (g_on_state)
      g_on_state(item, s);
  });
  if (!ok)
    ESP_LOGW(TAG, "Unparseable state event (%u bytes)", static_cast<unsigned>(data.size()));
}

void handle_event(const std::string &event, const std::string &data) {
  if (event == "ready") {
    ESP_LOGI(TAG, "Stream open, connection %s", data.c_str());
    {
      Lock l;
      g_conn_id = data;
      g_subscribed = false;
    }
    enqueue(new Job{Job::Kind::Subscribe, {}, {}});
  } else if (event == "alive") {
    // keepalive; receiving it already reset the silence timer
  } else if ((event.empty() || event == "message") && !data.empty()) {
    handle_states(data);
  }
}

// ------------------------------------------------------------------ stream ----

/* One connection, from open to whatever ends it. True if it delivered data,
   which resets the reconnect backoff. */
bool run_stream() {
  const appcfg::OpenHab cfg = appcfg::openhab();
  const uint32_t generation = g_generation.load();

  esp_http_client_handle_t client = make_client(cfg, cfg.url + "/rest/events/states", kConnectTimeoutMs);
  if (!client) {
    set_error("out of memory for the stream connection");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "text/event-stream");

  bool got_data = false;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    set_error(std::string("openHAB not reachable: ") + esp_err_to_name(err));
  } else {
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status == 401 || status == 403) {
      set_error("openHAB refused the stream (HTTP " + std::to_string(status) + "): check the token");
    } else if (status != 200) {
      set_error("openHAB stream failed (HTTP " + std::to_string(status) + ")");
    } else {
      esp_http_client_set_timeout_ms(client, kReadSliceMs);
      SseParser parser(kMaxEventBytes, handle_event);
      int64_t last_rx = esp_timer_get_time();
      char buf[1024];

      for (;;) {
        if (generation != g_generation.load() || !g_net_up.load())
          break;  // settings changed or the network went away: not an error

        const int n = esp_http_client_read(client, buf, sizeof(buf));
        const int64_t now = esp_timer_get_time();
        if (n > 0) {
          last_rx = now;
          got_data = true;
          if (!parser.feed(buf, static_cast<size_t>(n))) {
            set_error("oversized event on the stream");
            break;
          }
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
          if (now - last_rx > kSilenceUs) {
            set_error("stream silent for 30 s, reconnecting");
            break;
          }
        } else {
          set_error("openHAB closed the stream");
          break;
        }
      }
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  Lock l;
  g_conn_id.clear();
  g_subscribed = false;
  return got_data;
}

bool may_connect() { return g_net_up.load() && appcfg::openhab().configured(); }

void stream_task(void *) {
  int64_t backoff = kBackoffMinUs;
  for (;;) {
    if (!may_connect()) {
      /* Woken by on_network() or reconfigure(); the timeout is only a
         safety net. */
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
      continue;
    }

    const uint32_t generation = g_generation.load();
    if (run_stream())
      backoff = kBackoffMinUs;

    /* A reconfigure is a deliberate restart, not a failure to back off from. */
    if (generation != g_generation.load()) {
      backoff = kBackoffMinUs;
      continue;
    }
    {
      Lock l;
      g_reconnects++;
    }
    ESP_LOGI(TAG, "Reconnecting in %llds", backoff / 1000000);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff / 1000));
    backoff = std::min(backoff * 2, kBackoffMaxUs);
  }
}

// ------------------------------------------------------------------ worker ----

void do_subscribe() {
  std::string conn_id;
  std::vector<std::string> items;
  std::vector<std::string> groups;
  {
    Lock l;
    conn_id = g_conn_id;
    items = g_items;
    groups = g_groups;
  }
  /* The groups' members as far as known (members() fetches the others; members_changed() comes back here) --
     and the items their metadata names as "moving" (a cover group page's rows: which way each goes). */
  for (const auto &group : groups) {
    std::vector<Member> members;
    if (appoh::members(group, members))
      for (const auto &m : members) {
        items.push_back(m.name);
        for (const auto &moving : moving_items(m.metadata))
          items.push_back(moving);
      }
  }
  // an item whose own metadata (fetched by the page showing it) names one as "moving": that one too
  for (size_t i = 0, n = items.size(); i < n; i++) {
    std::map<std::string, Metadata> own;
    if (cached_metadata(items[i], own))
      for (const auto &moving : moving_items(own))
        items.push_back(moving);
  }
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
  {
    Lock l;
    g_sent = items;   // what the status counts
  }
  /* Nothing to do without a stream: the next "ready" queues another job. */
  if (conn_id.empty())
    return;

  cJSON *arr = cJSON_CreateArray();
  for (const auto &item : items)
    cJSON_AddItemToArray(arr, cJSON_CreateString(item.c_str()));
  char *text = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  if (!text)
    return;
  const std::string body = text;
  cJSON_free(text);

  /* application/json is required: openHAB 5.1 answers text/plain with a 500. */
  const int status = post(appcfg::openhab(), "/rest/events/states/" + conn_id, "application/json", body);
  if (status == 200) {
    ESP_LOGI(TAG, "Subscribed to %u items", static_cast<unsigned>(items.size()));
    Lock l;
    if (g_conn_id == conn_id)
      g_subscribed = true;
  } else if (status == 404) {
    /* The connection id is gone -- openHAB restarted under us. Start over. */
    set_error("openHAB no longer knows the stream, reconnecting");
    g_generation++;
    wake_stream();
  } else {
    set_error("subscribing failed (HTTP " + std::to_string(status) + ")");
  }
}

/* openHAB only accepts these in an item name, and the name becomes part of a
   URL path, so anything else is refused here rather than encoded. */
bool valid_item_name(const std::string &name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}

/* A command (POST /rest/items/<item>) goes to the devices behind the item; a
   state update (PUT .../state, 202) only sets the item -- what a sensor
   reports. */
void do_command(const Job &job) {
  const bool update = job.kind == Job::Kind::State;
  const int status = update ? post(appcfg::openhab(), "/rest/items/" + job.item + "/state", "text/plain", job.value,
                                   HTTP_METHOD_PUT)
                            : post(appcfg::openhab(), "/rest/items/" + job.item, "text/plain", job.value);
  const std::string value = job.secret ? "(" + std::to_string(job.value.size()) + " characters, not shown)" : job.value;
  if (status == 200 || status == 202) {
    ESP_LOGI(TAG, "%s %s -> %s", update ? "State" : "Command", job.item.c_str(), value.c_str());
    return;
  }
  const char *why = status == 400   ? "openHAB rejected the value"
                    : status == 404 ? "no such item"
                    : status == 401 ? "token rejected"
                    : status == 0   ? "openHAB not reachable"
                                    : "unexpected answer";
  set_error(std::string(update ? "state " : "command ") + job.item + " -> " + value + " failed: " + why + " (HTTP " + std::to_string(status) +
            ")");
}

/* A server forgets a TLS session a while after its full handshake, however often it was resumed since (openHAB's
   Jetty: a day; a proxy before it maybe sooner). An hour old, it is made anew while nothing waits on it -- by
   subscribing again, which changes nothing -- so that a command seldom pays for the full handshake. */
constexpr int64_t kSessionRenewUs = 3600 * 1000000LL;

void renew_session() {
  bool streaming;
  {
    Lock l;
    streaming = !g_conn_id.empty();
  }
  if (!g_keep || !streaming || !may_connect() || esp_timer_get_time() - g_keep_made_at < kSessionRenewUs)
    return;
  ESP_LOGI(TAG, "Renewing the TLS session");
  drop_keep();
  do_subscribe();
}

void worker_task(void *) {
  for (;;) {
    Job *job = nullptr;
    if (xQueueReceive(g_jobs, &job, pdMS_TO_TICKS(5 * 60 * 1000)) != pdPASS) {   // idle: maybe time to renew
      renew_session();
      continue;
    }
    if (!job)
      continue;
    if (may_connect()) {
      if (job->kind == Job::Kind::Subscribe)
        do_subscribe();
      else
        do_command(*job);
    }
    delete job;
  }
}

}  // namespace

bool begin(StateCallback on_state) {
  g_on_state = std::move(on_state);
  g_mux = xSemaphoreCreateMutex();
  g_jobs = xQueueCreate(kJobQueueLen, sizeof(Job *));
  if (!g_mux || !g_jobs)
    return false;

  /* Both may run a TLS handshake, which needs the stack. */
  if (xTaskCreate(stream_task, "oh_stream", 8192, nullptr, 5, &g_stream_task) != pdPASS ||
      xTaskCreate(worker_task, "oh_worker", 8192, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the openHAB tasks");
    return false;
  }
  return true;
}

void on_network(bool up) {
  if (g_net_up.exchange(up) != up)
    wake_stream();
}

void reconfigure() {
  {
    Lock l;
    g_cache.clear();
    g_predicted.clear();   // the old server's states: none may come back when a prediction expires
    g_sent.clear();        // the status counts against the new subscription
    g_last_error.clear();
  }
  g_generation++;
  wake_stream();
}

void subscribe(const std::vector<std::string> &items, const std::vector<std::string> &groups,
               const std::vector<std::string> &namespaces) {
  set_namespaces(namespaces);
  {
    Lock l;
    g_items = items;
    g_groups = groups;
  }
  enqueue(new Job{Job::Kind::Subscribe, {}, {}});
}

void members_changed() {
  bool any;
  {
    Lock l;
    any = !g_groups.empty() || !g_items.empty();
  }
  if (any)
    enqueue(new Job{Job::Kind::Subscribe, {}, {}});
}

namespace {

/* The state `command` will most likely give an item of the cached state's
   type, or false where that cannot be told (UP, TOGGLE, a dimmer's ON, ...). */
bool predicted(const ItemState &now, const std::string &command, ItemState &out) {
  out = now;
  char *end = nullptr;
  const double v = std::strtod(command.c_str(), &end);
  const bool number = !command.empty() && end && *end == '\0';
  if (now.type == "OnOff" && (command == "ON" || command == "OFF")) {
    out.state = out.display = command;
  } else if ((now.type == "Percent" || now.type == "Decimal" || now.type == "Quantity") && number) {
    out.numeric = v;
    out.has_numeric = true;
    out.state = command + (now.unit.empty() ? "" : " " + now.unit);
    out.display = now.type == "Percent" ? command + " %" : out.state;
  } else if ((now.type == "PlayPause" || now.type == "Player") && (command == "PLAY" || command == "PAUSE")) {
    out.state = out.display = command;
  } else if (now.type == "String") {
    out.state = out.display = command;
  } else {
    return false;
  }
  return true;
}

void predicted_changed(const std::string &item, const ItemState &next);

/* The state `item` will most likely have after `value` (a command, or a state the panel expects), in the cache at
   once and remembered with the one before, which comes back unless openHAB confirms in time. Under the lock. */
bool predict_locked(const std::string &item, const std::string &value, ItemState &next) {
  const auto it = g_cache.find(item);
  if (it == g_cache.end() || !predicted(it->second, value, next) || next.state == it->second.state)
    return false;
  if (!g_predicted.count(item))   // the state before the first of several: what comes back
    g_predicted[item].before = it->second;
  g_predicted[item].until = esp_timer_get_time() + (next.type == "String" ? kPredictTextUs : kPredictUs);
  it->second = next;
  return true;
}

/* Predictions no state from openHAB confirmed in time: the old state again. */
void expire_predictions(void *) {
  std::vector<std::pair<std::string, ItemState>> back;
  {
    Lock l;
    const int64_t now = esp_timer_get_time();
    for (auto it = g_predicted.begin(); it != g_predicted.end();) {
      if (now >= it->second.until) {
        g_cache[it->first] = it->second.before;
        back.emplace_back(it->first, it->second.before);
        it = g_predicted.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto &[item, s] : back) {
    ESP_LOGW(TAG, "%s: no answer to the command from openHAB -- its state as before", item.c_str());
    if (g_on_state)
      g_on_state(item, s);
  }
}

}  // namespace

bool send_command(const std::string &item, const std::string &command, bool secret) {
  if (!valid_item_name(item)) {
    set_error("invalid item name \"" + item + "\"");
    return false;
  }
  if (!enqueue(new Job{Job::Kind::Command, item, command, secret}))
    return false;
  if (secret)
    return true;   // not kept as the state it will likely have either
  // the state it will most likely have, at once
  ItemState next;
  bool changed;
  {
    Lock l;
    changed = predict_locked(item, command, next);
  }
  if (changed)
    predicted_changed(item, next);
  return true;
}

bool expect(const std::string &item, const std::string &state) {
  ItemState next;
  {
    Lock l;
    if (!predict_locked(item, state, next))
      return false;
  }
  predicted_changed(item, next);
  return true;
}

namespace {
void predicted_changed(const std::string &item, const ItemState &next) {
  if (!g_predict_timer) {
    const esp_timer_create_args_t args = {.callback = expire_predictions, .arg = nullptr,
                                          .dispatch_method = ESP_TIMER_TASK, .name = "oh_predict",
                                          .skip_unhandled_events = true};
    if (esp_timer_create(&args, &g_predict_timer) == ESP_OK)
      esp_timer_start_periodic(g_predict_timer, 1000000);
  }
  if (g_on_state)
    g_on_state(item, next);
}
}  // namespace

bool send_state(const std::string &item, const std::string &state) {
  if (!valid_item_name(item)) {
    set_error("invalid item name \"" + item + "\"");
    return false;
  }
  return enqueue(new Job{Job::Kind::State, item, state});
}

bool state(const std::string &item, ItemState &out) {
  Lock l;
  const auto it = g_cache.find(item);
  if (it == g_cache.end())
    return false;
  out = it->second;
  return true;
}

Status status() {
  Lock l;
  Status s;
  s.connected = !g_conn_id.empty() && g_subscribed;
  // the items subscribed (the groups' members too) and those of them with a state -- not what else the cache holds
  const std::vector<std::string> &all = g_sent.empty() ? g_items : g_sent;
  s.subscribed = all.size();
  s.received = static_cast<size_t>(std::count_if(all.begin(), all.end(), [](const std::string &i) { return g_cache.count(i) > 0; }));
  s.reconnects = g_reconnects;
  s.last_error = g_last_error;
  return s;
}

std::string states_json() {
  cJSON *root = cJSON_CreateObject();
  {
    Lock l;
    for (const auto &[name, s] : g_cache) {
      cJSON *o = cJSON_AddObjectToObject(root, name.c_str());
      cJSON_AddStringToObject(o, "state", s.state.c_str());
      cJSON_AddStringToObject(o, "display", s.display.c_str());
      cJSON_AddStringToObject(o, "type", s.type.c_str());
      if (!s.unit.empty())
        cJSON_AddStringToObject(o, "unit", s.unit.c_str());
      if (s.has_numeric)
        cJSON_AddNumberToObject(o, "numeric", s.numeric);
    }
  }
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  std::string out = text ? text : "{}";
  cJSON_free(text);
  return out;
}

}  // namespace appoh
