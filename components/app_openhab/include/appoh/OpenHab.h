/*
   openHAB REST client.

   The panel talks to openHAB directly instead of through an MQTT bridge: item
   states arrive over the server-sent-events stream /rest/events/states, already
   formatted by openHAB (displayState), and commands go out as plain POSTs to
   /rest/items/<name>.

   Protocol (openHAB 5.1, see openhab-core SseResource):
     GET  /rest/events/states            -> "event: ready", data = connection id
     POST /rest/events/states/<id>       JSON array of item names; answered on the
                                         stream with one snapshot of all of them,
                                         then one event per state change
     "event: alive" every 10 s           keepalive; silence means a dead link
     POST /rest/items/<name>             text/plain command ("ON", "UP", "42", ...)
*/

#pragma once

#include "appcfg/Config.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace appoh {

/* One item state as openHAB reports it. */
struct ItemState {
  std::string state;    // raw, e.g. "80", "ON", "21.5 °C", "NULL"
  std::string display;  // formatted by openHAB (pattern, MAP transform, option label); falls back to state
  std::string type;     // "OnOff", "Percent", "Quantity", "Decimal", "DateTime", "HSB", "String", "UnDef", ...
  std::string unit;     // only for quantities, e.g. "W"
  double numeric = 0;   // valid if has_numeric (numbers, percent, HSB brightness)
  bool has_numeric = false;
  bool undefined() const { return type == "UnDef"; }
};

/* Called on the client's stream task for every state that arrives, the
   snapshot after (re)subscribing included. Keep it short. */
using StateCallback = std::function<void(const std::string &item, const ItemState &state)>;

/* Starts the client tasks. Nothing connects until on_network(true). */
bool begin(StateCallback on_state = {});

/* The station link went up or down. The stream only runs while it is up and
   an openHAB URL is configured. */
void on_network(bool up);

/* The stored openHAB settings changed: drop the stream and reconnect. */
void reconfigure();

/* The items whose states the panel needs, and the groups whose members' it
   needs too (members(): as they come, and again when they change), with the
   namespaces of their metadata the pages read (only those are asked for and
   kept). Replaces the previous sets and is re-sent on every reconnect;
   openHAB answers with a fresh snapshot. */
void subscribe(const std::vector<std::string> &items, const std::vector<std::string> &groups = {},
               const std::vector<std::string> &namespaces = {});

/* Queues a command; returns false if the queue is full. Sent on a separate
   connection, so it never waits for the stream. `secret` (a code typed on the
   keypad page): its value neither logged nor kept as the item's expected state. */
bool send_command(const std::string &item, const std::string &command, bool secret = false);

/* Queues a state update: sets the item without commanding anything behind it
   -- for values the panel measures (its temperature sensor). */
bool send_state(const std::string &item, const std::string &state);

/* The last state received for an item; false if none has arrived. */
bool state(const std::string &item, ItemState &out);

struct Status {
  bool connected = false;      // stream open and subscribed
  size_t subscribed = 0;       // items requested
  size_t received = 0;         // items with a state in the cache
  uint32_t reconnects = 0;
  std::string last_error;
};
Status status();

/* All cached states as a JSON object {item: {state, display, type, ...}}, for
   the web interface. */
std::string states_json();

/* An item from openHAB's item list, for the configuration editor. */
struct ItemInfo {
  std::string name;
  std::string label;
  std::string type;  // e.g. "Switch", "Number:Temperature", "Group"
};

/* Searches openHAB's item list: every whitespace-separated word of `query`
   must occur in the name or the label (ASCII case-insensitive), and the type
   must start with `type` if given. Returns at most `limit` items from the
   `offset`th match on (in the list's order) and the number of all matches. The list is fetched once and cached for a few
   minutes (`refresh` forces a new fetch); blocks while fetching. */
bool search_items(const std::string &query, const std::string &type, size_t limit, std::vector<ItemInfo> &out,
                  size_t &matches, std::string &error, bool refresh = false, size_t offset = 0);

/* Sets the state the item will most likely have soon (as a command to it
   does, appoh::send_command), without sending anything -- e.g. what plays
   after a stream was sent to another item. Undone when openHAB says nothing
   of the item in time. False where it cannot be told. */
bool expect(const std::string &item, const std::string &state);

/* An option of an item: what to send (or the state it stands for) and its
   label; and what the item's "ohpanel" metadata gives it by its label --
   `icons="label=icon,..."`, `images="label=URL or /path,..."`. */
struct Option {
  std::string value;
  std::string label;
  std::string icon;   // a Material Design Icons name
  std::string image;  // a logo's code (tools/station_logo.py), drawn by the panel
  std::string short_name;   // for a tile; the label if none
  std::string name;         // the long name, e.g. a station's full one; the label if none
  bool operator==(const Option &) const = default;
};

/* The item's command options, else its state options (commandDescription /
   stateDescription), from a cache; false while not known yet. Never blocks:
   an item not cached, or cached over 5 minutes ago, is fetched in the
   background, and the listener hears when it has come (on another task). */
bool options(const std::string &item, std::vector<Option> &out, bool refresh = false);   // refresh: fetched again now
void set_options_listener(std::function<void(const std::string &item)> listener);

/* The item's range -- its stateDescription's minimum, maximum and step (a lamp's
   colour temperature, a thermostat's target) --, from the same cache, fetched as options()
   is; false while not known, or when openHAB gives none. */
struct Limits {
  bool has_min = false, has_max = false, has_step = false;
  double min = 0, max = 0, step = 0;
  bool operator==(const Limits &) const = default;
};
bool limits(const std::string &item, Limits &out);

/* A group's members (its direct ones), by label, from the same cache,
   fetched as options() is -- the listener hears when they have come or
   changed --; false while not known. openHAB tells nobody when a group gains
   a member: whoever shows them asks afresh (`refresh`). */
struct Metadata {
  std::string value;
  std::map<std::string, std::string> config;   // numbers and true/false written as text
  bool operator==(const Metadata &) const = default;
};
/* The items an item's metadata (any namespace) names as "moving" -- one saying which way a cover goes --, which
   are subscribed with it. */
inline std::vector<std::string> moving_items(const std::map<std::string, Metadata> &metadata) {
  std::vector<std::string> out;
  for (const auto &[ns, md] : metadata)
    if (const auto it = md.config.find("moving"); it != md.config.end() && !it->second.empty())
      out.push_back(it->second);
  return out;
}
struct Member {
  std::string name;
  std::string label;   // the item's label; its name if it has none
  std::map<std::string, Metadata> metadata;   // by namespace, e.g. "plant"
  bool operator==(const Member &) const = default;
};
bool members(const std::string &group, std::vector<Member> &out, bool refresh = false);

/* An item's own metadata (the namespaces the pages read, as members() has them), by namespace, from the same
   cache -- fetched when not cached (and asked afresh with `refresh`); false while not known. */
bool metadata(const std::string &item, std::map<std::string, Metadata> &out, bool refresh = false);

/* A file from the web (a cover picture): an http(s) address, or a path on
   the openHAB server ("/..."), which -- like an address on it -- goes with
   the configured token. Blocking; false (and why in `error`) when it did
   not come, or came larger than `limit` bytes. */
bool fetch(const std::string &url, size_t limit, std::string &body, std::string *error = nullptr);

/* openHAB over https on a host, looked for on `port` (when it is not one of
   plain http's), 443 and 8443, whatever protocol the settings use: where it
   answers, whether the built-in CAs vouch for its certificate, and else the
   certificate the panel could trust (the self-signed one its chain ends in).
   Blocks for a few seconds (src/Discover.cpp). */
struct Discovery {
  bool found = false;            // openHAB answers over https
  int port = 0;
  std::string version;           // its version, where it tells it
  bool public_ca = false;        // the built-in CAs vouch for it: https needs nothing else
  bool importable = false;       // its chain ends in a self-signed certificate: pem, to trust
  bool self_signed = false;      // the server's certificate itself is that one
  std::string pem, subject, valid_to, fingerprint;   // of that certificate (SHA-256, AA:BB:...)
  std::vector<std::string> names;   // what the server's certificate is issued for
  bool name_ok = false;          // the host is one of them
  std::string error;
};
Discovery discover(const std::string &host, int port, bool only = false);   // only: that port alone

/* openHAB servers in the network, as they announce themselves by mDNS: one
   entry per address, with its http and https port (0: none). Blocks for a
   few seconds (src/Scan.cpp). */
struct Found {
  std::string name;       // the instance's name ("openhab", "openhab (2)")
  std::string hostname;   // "....local"
  std::string address;    // its IPv4 address, else "[IPv6]"
  int http_port = 0, https_port = 0;
  Discovery tls;          // with check: what its https port answers (discover)
  bool http_ok = false;   // with check: openHAB answers on its http port
  std::string version;    // with check: its version, where it says
};
std::vector<Found> scan(bool check = false);   // check: each https port looked at too

/* What a connection test found out. */
struct ProbeResult {
  bool ok = false;
  int http_status = 0;  // of the request that decided the outcome; 0 if none got through
  std::string error;    // why it failed, for the user
  std::string version;  // openHAB runtime version, e.g. "5.1.4"
  std::string locale;   // server locale, which is what displayState is formatted in
  std::string timezone;
};

/* Checks that the server answers, that it is openHAB, and that the
   credentials may read items. Blocks for up to a few seconds. */
ProbeResult probe(const appcfg::OpenHab &cfg);

/* "http://host:8080/" -> "http://host:8080"; empty if it is not an http(s) URL. */
std::string normalize_url(const std::string &url);

}  // namespace appoh
