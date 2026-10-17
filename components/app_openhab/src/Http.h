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

/* The metadata namespaces to ask openHAB for, besides "ohpanel" (Options.cpp). */
void set_namespaces(const std::vector<std::string> &namespaces);

Response http_get(const appcfg::OpenHab &cfg, const std::string &path, size_t limit, int timeout_ms = 0);

}  // namespace appoh
