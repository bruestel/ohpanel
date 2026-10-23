/*
   The display on the device: one task owns the Engine and feeds it, in
   order, everything that concerns the display -- messages from the panel's
   screen, item states, configuration changes, network status, buttons.
   Every function here only queues and returns.
*/

#pragma once

#include "appdisplay/Engine.h"
#include "nextion/Display.h"

#include <functional>
#include <string>

namespace appdisplay {

struct Hooks {
  std::function<void(int index, bool on)> relay;
  std::function<void()> restart;
  /* The panel's values for the info page; the display's own are added here. */
  std::function<SystemInfo()> system;
  /* A display.screen number its openHAB item set, to keep in the configuration. */
  std::function<void(const std::string &key, int value)> remember;
  /* The display switched off and on again (its power, the board's pin):
     what brings back one stuck in an interrupted upload -- a restart of the
     ESP alone does not reach it. */
  std::function<void()> power_cycle;
};

bool begin(const nextion::Config &uart, Hooks hooks);

void config_changed(const std::string &json);
void state_changed(const std::string &item);
void status(const PanelStatus &status);
void start();
void hardware_button(int index);
void relay_state(int index, bool on);

/* What the display said about itself (answer to "connect", asked each time
   the link finds it, Display.cpp); false until it has answered. */
bool display_info(nextion::DeviceInfo &out);

/* The panel variant this firmware is built for, its display design with it (tools/variant.py): "eu" (the NSPanel
   EU), "us" (the NSPanel US). */
const char *variant();

/* The rate of the serial link to the display now (921600 with our design;
   another only with an older or another one). */
int link_baud();

/* The display design's version (the global design_ver of its Program.s,
   boards/nspanel/nextion/ohpanel_hmi.py) against the one in the manifest this firmware was
   built with. */
struct DesignCheck {
  bool known = false;    // the display has answered
  std::string expected;  // from the manifest
  std::string actual;    // from the display; empty: it has no version (an older design)
  bool current() const { return known && actual == expected; }
};
DesignCheck design_check();

/* Writing a display firmware (.tft) -- see nextion::Display. Called from the
   web server's task; the display logic pauses meanwhile. tft_begin switches
   the display off and on first (about 2.3 s), so the upload meets it fresh. */
bool tft_begin(size_t size, int baud, std::string *error);
bool tft_write(const uint8_t *data, size_t len, std::string *error);
bool tft_end(std::string *error);
/* One command straight to the display (the diagnostics page's console). */
void send_raw(const std::string &command);
void log_commands(bool on);  // every command to the display in the log
bool logging_commands();
void tft_abort();

}  // namespace appdisplay
