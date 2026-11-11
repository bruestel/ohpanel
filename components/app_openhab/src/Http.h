/*
   Small HTTP helpers shared inside the component.
*/

#pragma once

#include "appcfg/Config.h"

#include <esp_err.h>
#include <esp_http_client.h>

#include <string>

namespace appoh {

struct Response {
  int status = 0;
  std::string body;
  esp_err_t err = ESP_OK;
};

/* The certificate check of the settings (appcfg::Tls) on a client's config.
   With Tls::Custom the config points into cfg.ca: cfg must outlive the client. */
void apply_tls(esp_http_client_config_t &hc, const appcfg::OpenHab &cfg);

/* A blocking GET with the configured credentials; the body is refused beyond
   `limit` bytes (err = ESP_ERR_INVALID_SIZE). `timeout_ms` 0: the usual. */
/* The groups' members are known anew (Options.cpp): the subscription with them (Client.cpp). */
void members_changed();
/* An item's own metadata if it is cached -- nothing fetched (the subscription asks it of every item). */
bool cached_metadata(const std::string &item, std::map<std::string, Metadata> &out);

/* openHAB answers again (subscribed): the items whose fetch failed may be fetched at once, not after their rest
   (Options.cpp). */
void fetches_may_retry();

/* The metadata namespaces to ask openHAB for, besides "ohpanel" (Options.cpp). */
void set_namespaces(const std::vector<std::string> &namespaces);

Response http_get(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms = 0);

/* The same from the configured openHAB, resuming the TLS session of the GET before (no keep-alive: the connection
   is closed after each, only the session is kept): a resumed handshake instead of a full one, which takes this
   chip a second or more -- the item details (options, labels, members, metadata) come one after the other. */
Response http_get_resumed(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms = 0);

/* Two openHAB settings reach the same server the same way (address, token, certificate check). */
bool same_settings(const appcfg::OpenHab &a, const appcfg::OpenHab &b);

}  // namespace appoh
