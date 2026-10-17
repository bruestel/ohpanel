/*
   The panel's own temperature sensor.
*/

#include "temperature.h"

#include "appcfg/Config.h"
#include "appoh/OpenHab.h"
#include "board.h"
#include "ntc.h"

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace temperature {
namespace {

const char *const TAG = "temp";

constexpr adc_channel_t kChannel = ADC_CHANNEL_2;  // GPIO38
constexpr int64_t kSampleUs = 2 * 1000000LL;
constexpr int64_t kReportUs = 60 * 1000000LL;
constexpr int64_t kReportAnywayUs = 10 * 60 * 1000000LL;
constexpr int kReadsPerSample = 16;  // averaged against ADC noise
constexpr double kSmoothing = 0.2;   // share of a new sample in the value

adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
double g_raw = NAN;
int64_t g_next_sample = 0;
int64_t g_next_report = 0;
int64_t g_last_report = 0;
std::string g_reported;

/* The pin voltage in millivolts, averaged; negative if the ADC failed. */
int read_mv() {
  int sum = 0;
  for (int i = 0; i < kReadsPerSample; i++) {
    int raw = 0;
    if (adc_oneshot_read(g_adc, kChannel, &raw) != ESP_OK)
      return -1;
    sum += raw;
  }
  int mv = 0;
  if (adc_cali_raw_to_voltage(g_cali, sum / kReadsPerSample, &mv) != ESP_OK)
    return -1;
  return mv;
}

void report(int64_t now) {
  const appcfg::Sensor s = appcfg::sensor();
  const double c = celsius();
  if (s.item.empty() || std::isnan(c) || !appoh::status().connected)
    return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", c);
  /* A plain number: a Number item takes it, a Number:Temperature item reads
     it in its own unit (degrees Celsius in a metric setup). */
  if (buf == g_reported && now - g_last_report < kReportAnywayUs)
    return;
  if (appoh::send_state(s.item, buf)) {
    g_reported = buf;
    g_last_report = now;
  }
}

}  // namespace

bool begin() {
  adc_oneshot_unit_init_cfg_t unit = {};
  unit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &g_adc) != ESP_OK) {
    ESP_LOGW(TAG, "ADC not available -- no temperature");
    return false;
  }
  adc_oneshot_chan_cfg_t chan = {};
  chan.atten = ADC_ATTEN_DB_12;  // the full 0..3.3 V of the divider
  chan.bitwidth = ADC_BITWIDTH_DEFAULT;
  adc_cali_line_fitting_config_t cali = {};
  cali.unit_id = ADC_UNIT_1;
  cali.atten = ADC_ATTEN_DB_12;
  cali.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(g_adc, kChannel, &chan) != ESP_OK ||
      adc_cali_create_scheme_line_fitting(&cali, &g_cali) != ESP_OK) {
    ESP_LOGW(TAG, "ADC channel or calibration not available -- no temperature");
    adc_oneshot_del_unit(g_adc);
    g_adc = nullptr;
    return false;
  }
  return true;
}

void tick(int64_t now) {
  if (!g_adc || now < g_next_sample)
    return;
  g_next_sample = now + kSampleUs;
  const int mv = read_mv();
  const double c = mv < 0 ? NAN : ntc::celsius(mv);
  if (std::isnan(c)) {
    if (!std::isnan(g_raw))
      ESP_LOGW(TAG, "No plausible reading (%d mV) -- sensor missing?", mv);
    g_raw = NAN;
  } else {
    g_raw = std::isnan(g_raw) ? c : g_raw + kSmoothing * (c - g_raw);
  }
  if (now >= g_next_report) {
    g_next_report = now + kReportUs;
    report(now);
  }
}

double raw_celsius() { return g_raw; }

double celsius() { return std::isnan(g_raw) ? NAN : g_raw + appcfg::sensor().offset_tenths / 10.0; }

}  // namespace temperature
