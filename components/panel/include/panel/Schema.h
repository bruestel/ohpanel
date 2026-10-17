/*
   Validation of the panel configuration against the schema in
   docs/panel-config.md. Free of ESP-IDF (cJSON only), so it runs in the host
   tests as well.
*/

#pragma once

#include <string>
#include <vector>

namespace panel {

/* The configuration version this firmware understands. */
inline constexpr int kSchemaVersion = 1;

struct Issue {
  std::string path;  // e.g. "pages[2].buttons[3].open"
  std::string message;
};

/* True if the document is a valid configuration. Otherwise `issues` lists
   what is wrong, at most kMaxIssues of it. */
bool validate(const std::string &json, std::vector<Issue> &issues);

inline constexpr size_t kMaxIssues = 25;

}  // namespace panel
