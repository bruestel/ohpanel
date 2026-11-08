/*
   The panel's buzzer.
*/

#include "buzzer.h"

#include "board.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>

namespace buzzer {
namespace {

const char *const TAG = "buzzer";

constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
/* 12 bits: up to 19.5 kHz from the 80 MHz clock, more than any tone needs. A
   piezo is loudest at half the period (2048). */
constexpr ledc_timer_bit_t kBits = LEDC_TIMER_12_BIT;
constexpr uint32_t kHalf = 1u << (kBits - 1);

TaskHandle_t g_task = nullptr;
SemaphoreHandle_t g_mx = nullptr;
std::vector<Note> g_next;   // what play() handed over, under g_mx
int g_next_volume = 0;

/* The volume as the square wave's duty: the ear hears the sound pressure on a
   logarithmic scale, so the duty grows with its square (100: half the period). */
uint32_t duty_of(int volume) {
  const uint32_t v = std::clamp(volume, 0, 100);
  return v ? std::max<uint32_t>(1, kHalf * v * v / 10000) : 0;
}

void silent() {
  ledc_set_duty(kMode, kChannel, 0);
  ledc_update_duty(kMode, kChannel);
}

void task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::vector<Note> notes;
    int volume;
    xSemaphoreTake(g_mx, portMAX_DELAY);
    notes.swap(g_next);
    volume = g_next_volume;
    xSemaphoreGive(g_mx);
    for (const Note &n : notes) {
      const uint32_t duty = duty_of((volume * n.level + 99) / 100);
      if (n.hz && duty) {
        ledc_set_freq(kMode, kTimer, n.hz);
        ledc_set_duty(kMode, kChannel, duty);
        ledc_update_duty(kMode, kChannel);
      } else {
        silent();
      }
      // a new tone cuts this one off: it plays at once
      if (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(std::max<int>(1, n.ms))) > 0) {
        xTaskNotifyGive(g_task);   // given back: the next round of the loop takes it
        break;
      }
    }
    silent();
  }
}

}  // namespace

bool begin() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = kMode;
  timer.duty_resolution = kBits;
  timer.timer_num = kTimer;
  timer.freq_hz = 2700;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ledc_channel_config_t channel = {};
  channel.gpio_num = board::PIN_BUZZER;
  channel.speed_mode = kMode;
  channel.channel = kChannel;
  channel.timer_sel = kTimer;
  channel.duty = 0;
  if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
    ESP_LOGE(TAG, "No LEDC for the buzzer");
    return false;
  }
  g_mx = xSemaphoreCreateMutex();
  if (!g_mx || xTaskCreate(task, "buzzer", 2048, nullptr, 5, &g_task) != pdPASS) {
    ESP_LOGE(TAG, "No memory for the buzzer's task");
    g_task = nullptr;
    return false;
  }
  return true;
}

void play(const std::vector<Note> &notes, int volume) {
  if (!g_task || notes.empty() || volume <= 0)
    return;
  xSemaphoreTake(g_mx, portMAX_DELAY);
  g_next = notes;
  g_next_volume = volume;
  xSemaphoreGive(g_mx);
  xTaskNotifyGive(g_task);
}

}  // namespace buzzer
