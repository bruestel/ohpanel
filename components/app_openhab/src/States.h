/*
   Decoding of openHAB's item state events. Free of ESP-IDF (cJSON only) so it
   can be tested on a host.
*/

#pragma once

#include "appoh/OpenHab.h"

#include <functional>
#include <string>

namespace appoh {

/* A state event is {"Item": {"state": ..., "displayState": ..., ...}, ...}: the
   snapshot after subscribing carries every item, a change carries one. Calls
   the handler once per item; false if the text is not such an object. */
bool parse_states(const std::string &data, const std::function<void(const std::string &, const ItemState &)> &handler);

}  // namespace appoh
