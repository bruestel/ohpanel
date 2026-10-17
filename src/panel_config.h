/*
   The panel configuration: pages, buttons, entities -- everything that was
   compiled into the Arduino firmware per room (BUILD_TARGET) and is now one
   JSON document on the storage partition, edited through the web interface.
   The schema is documented in docs/panel-config.md.
*/

#pragma once

#include <string>
#include <vector>

namespace panel_config {

/* The stored document, or the built-in default when none is stored. */
std::string load();

/* Validates and stores. On failure nothing changes and *error says why. */
bool save(const std::string &json, std::string *error);

/* Removes the stored document, so load() returns the default again. */
bool reset();

/* Every openHAB item the configuration refers to: the string value of any
   member named "item", at any depth. The convention holds whatever shape the
   schema takes, so the subscription never has to follow it. Sorted, unique. */
std::vector<std::string> items();

/* The groups whose members entity pages list ("group"), and the namespaces of
   their metadata the pages read ("metadata"). Sorted, unique. */
std::vector<std::string> groups();
std::vector<std::string> namespaces();

/* Keeps a number of display.screen (brightness, idle_brightness, idle_after)
   that its openHAB item set: as {"value", "item"} where an item is linked,
   as the number otherwise. Without telling the display, which has it. */
bool remember_screen(const std::string &key, int value);

}  // namespace panel_config
