#pragma once

#include "appoh/OpenHab.h"

#include <string>

namespace appdisplay {

/* openHAB's icon for an item -- its category ("door", "oh:classic:light", "iconify:mdi:door-open") -- as the name of
   one of the panel's icons, by the item's state as openHAB's dynamic icons go (on/off, open/closed, a number's step);
   "" for a category the table has not (src/OhIcons.cpp, made by tools/gen_oh_icons.py). */
std::string oh_icon(const std::string &category, const appoh::ItemState *state);

}  // namespace appdisplay
