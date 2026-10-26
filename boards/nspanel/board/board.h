/*
   NSPanel (EU) pin map and the few board helpers the firmware needs.

   From the Arduino firmware on main and the published teardowns. GPIO5 and
   GPIO18 belong to the PSRAM and must not be used for anything else.
*/

#pragma once

#include <driver/gpio.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace board {

/* Front buttons, active low. */
constexpr gpio_num_t PIN_KEY1 = GPIO_NUM_14;
constexpr gpio_num_t PIN_KEY2 = GPIO_NUM_27;

/* Relays, active high. */
constexpr gpio_num_t PIN_RELAY1 = GPIO_NUM_22;
constexpr gpio_num_t PIN_RELAY2 = GPIO_NUM_19;

/* Display power: LOW switches the Nextion on. */
constexpr gpio_num_t PIN_DISPLAY_POWER = GPIO_NUM_4;

/* Display UART (Nextion TX -> ESP RX). */
constexpr gpio_num_t PIN_DISPLAY_RX = GPIO_NUM_17;
constexpr gpio_num_t PIN_DISPLAY_TX = GPIO_NUM_16;

/* Temperature sensor: an NTC (10 kOhm at 25 C, B 3950) between GPIO38 (ADC1
   channel 2) and ground, 11.2 kOhm from 3.3 V -- the values the ESPHome
   configurations for this panel use. See temperature.cpp. */
constexpr gpio_num_t PIN_NTC = GPIO_NUM_38;

inline void init() {
  gpio_config_t in = {};
  in.pin_bit_mask = (1ULL << PIN_KEY1) | (1ULL << PIN_KEY2);
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&in);

  /* Relays off until the settings are read and their last state restored
     (app_main), a few milliseconds later -- after a power cut. After a
     restart their pins are still held at the level they had (main.cpp
     set_relay): what is set here does not reach them, the relays stay. */
  gpio_config_t out = {};
  out.pin_bit_mask = (1ULL << PIN_RELAY1) | (1ULL << PIN_RELAY2) | (1ULL << PIN_DISPLAY_POWER);
  out.mode = GPIO_MODE_OUTPUT;
  gpio_config(&out);
  gpio_set_level(PIN_RELAY1, 0);
  gpio_set_level(PIN_RELAY2, 0);

  /* Display on: it shows its splash page until the firmware takes over. A
     restart that was not a power-on (the web interface's, an update, a
     crash) left it on, its last page frozen until then: off for a moment
     first, so it starts as at power-on. */
  if (esp_reset_reason() != ESP_RST_POWERON) {
    gpio_set_level(PIN_DISPLAY_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  gpio_set_level(PIN_DISPLAY_POWER, 0);
}

inline bool key_pressed(gpio_num_t pin) { return gpio_get_level(pin) == 0; }

}  // namespace board
