/*
   Knowing what went wrong on a device you cannot reach.
*/

#include "diagnostics.h"

#include "appcfg/Config.h"

#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace diagnostics {
namespace {

const char *const TAG = "diag";

esp_reset_reason_t g_reason = ESP_RST_UNKNOWN;
bool g_registered = false;
size_t g_coredump_size = 0;
/* What the stored core dump says about its crash, read once at boot; null if
   there is none or it cannot be read. */
esp_core_dump_summary_t *g_summary = nullptr;

/* Xtensa EXCCAUSE values a crash in this firmware can plausibly show. */
const char *exception_text(uint32_t cause) {
  switch (cause) {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "";
  }
}

/* The summary as JSON: task, program counter, exception, backtrace, and the
   ELF hash of the firmware that crashed -- which names the .elf needed to
   decode the dump (tools/archive_elf.py files them under that hash). */
std::string summary_json() {
  if (!g_summary)
    return "null";
  const esp_core_dump_summary_t &s = *g_summary;
  char running[CONFIG_APP_RETRIEVE_LEN_ELF_SHA] = {};
  esp_app_get_elf_sha256(running, sizeof(running));
  char task[sizeof(s.exc_task) + 1] = {};
  for (size_t i = 0; i < sizeof(s.exc_task) && s.exc_task[i]; i++)  // nothing that would need escaping
    task[i] = (s.exc_task[i] == '"' || s.exc_task[i] == '\\' || s.exc_task[i] < 0x20) ? '?' : s.exc_task[i];
  const char *elf = reinterpret_cast<const char *>(s.app_elf_sha256);

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"task\":\"%s\",\"pc\":\"0x%08lx\",\"cause\":%lu,\"cause_text\":\"%s\",\"vaddr\":\"0x%08lx\","
           "\"elf_sha256\":\"%.*s\",\"same_firmware\":%s,\"backtrace_corrupted\":%s,\"backtrace\":[",
           task, static_cast<unsigned long>(s.exc_pc), static_cast<unsigned long>(s.ex_info.exc_cause),
           exception_text(s.ex_info.exc_cause), static_cast<unsigned long>(s.ex_info.exc_vaddr),
           static_cast<int>(sizeof(s.app_elf_sha256)), elf,
           strncmp(elf, running, sizeof(running)) == 0 ? "true" : "false", s.exc_bt_info.corrupted ? "true" : "false");
  std::string o = buf;
  const uint32_t depth = std::min<uint32_t>(s.exc_bt_info.depth, sizeof(s.exc_bt_info.bt) / sizeof(s.exc_bt_info.bt[0]));
  for (uint32_t i = 0; i < depth; i++) {
    snprintf(buf, sizeof(buf), "%s\"0x%08lx\"", i ? "," : "", static_cast<unsigned long>(s.exc_bt_info.bt[i]));
    o += buf;
  }
  return o + "]}";
}
size_t g_coredump_addr = 0;

const char *reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic or exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

}  // namespace

void begin() {
  g_reason = esp_reset_reason();

  /* React rather than assume. The transmit power runs unrestricted until a
     brownout actually happens, because capping it pre-emptively costs range
     and would hide whether this supply is affected at all. One brownout is evidence; step down and say so. */
  if (g_reason == ESP_RST_BROWNOUT) {
    const int current = appcfg::tx_power_dbm();
    const int next = (current == 0) ? 17 : (current > 8 ? current - 3 : current);
    if (next != current) {
      appcfg::set_tx_power_dbm(next);
      ESP_LOGE(TAG, "Brownout: the supply sagged, most likely under the WiFi transmit "
                    "peak. Transmit power reduced to %d dBm for the next boot.", next);
    } else {
      ESP_LOGE(TAG, "Brownout at %d dBm, already at the lowest step. The supply is "
                    "probably not the WiFi peak -- check the mains supply.", current);
    }
  }
  else if (crashed_last_boot())
    ESP_LOGE(TAG, "Last restart: %s", reason_text(g_reason));
  else
    ESP_LOGI(TAG, "Boot reason: %s", reason_text(g_reason));

  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size) {
    g_coredump_addr = addr;
    g_coredump_size = size;
    g_summary = static_cast<esp_core_dump_summary_t *>(calloc(1, sizeof(esp_core_dump_summary_t)));
    if (g_summary && esp_core_dump_get_summary(g_summary) != ESP_OK) {
      free(g_summary);
      g_summary = nullptr;
    }
    if (g_summary)
      ESP_LOGW(TAG, "Crash in task %.16s at 0x%08lx (EXCCAUSE %lu %s)", g_summary->exc_task,
               static_cast<unsigned long>(g_summary->exc_pc), static_cast<unsigned long>(g_summary->ex_info.exc_cause),
               exception_text(g_summary->ex_info.exc_cause));
    ESP_LOGW(TAG, "A core dump from an earlier crash is stored (%u bytes); "
                  "download it from the web interface before erasing.",
             static_cast<unsigned>(size));
  }

  /* The task watchdog is already initialised by the IDF; adding this task means
     a main loop that stops running reboots the device instead of leaving it
     nominally alive but doing nothing. That distinction matters in a wall box:
     an unresponsive device looks exactly like a dead one, but only one of them
     recovers by itself. */
  esp_err_t err = esp_task_wdt_add(nullptr);
  g_registered = (err == ESP_OK);
  if (!g_registered)
    ESP_LOGW(TAG, "Could not register with the task watchdog: %s", esp_err_to_name(err));
}

void feed() {
  if (g_registered)
    esp_task_wdt_reset();
}

const char *boot_reason() { return reason_text(g_reason); }

bool crashed_last_boot() {
  return g_reason == ESP_RST_PANIC || g_reason == ESP_RST_TASK_WDT ||
         g_reason == ESP_RST_INT_WDT || g_reason == ESP_RST_WDT ||
         g_reason == ESP_RST_BROWNOUT;
}

size_t coredump_size() { return g_coredump_size; }

bool read_coredump(size_t offset, void *buf, size_t len) {
  if (!g_coredump_size || offset + len > g_coredump_size)
    return false;
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part)
    return false;
  return esp_partition_read(part, offset, buf, len) == ESP_OK;
}

bool erase_coredump() {
  if (esp_core_dump_image_erase() != ESP_OK)
    return false;
  g_coredump_size = 0;
  free(g_summary);
  g_summary = nullptr;
  ESP_LOGI(TAG, "Core dump erased");
  return true;
}

std::string health_json() {
  char buf[192];
  std::string o = "{";

  snprintf(buf, sizeof(buf), "\"boot_reason\":\"%s\",\"crashed\":%s,",
           reason_text(g_reason), crashed_last_boot() ? "true" : "false");
  o += buf;

  snprintf(buf, sizeof(buf),
           "\"uptime_s\":%lld,\"heap\":%u,\"heap_min\":%u,\"heap_largest\":%u,",
           static_cast<long long>(esp_timer_get_time() / 1000000),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  o += buf;

  o += "\"crash\":" + summary_json() + ",";
  snprintf(buf, sizeof(buf), "\"coredump\":%u,\"watchdog\":%s,\"tx_power\":%d,\"cpu_mhz\":%u,\"cores\":%d,",
           static_cast<unsigned>(g_coredump_size), g_registered ? "true" : "false",
           appcfg::tx_power_dbm(), static_cast<unsigned>(esp_rom_get_cpu_ticks_per_us()), portNUM_PROCESSORS);
  o += buf;

  /* Stack headroom per task. The interesting number is the minimum ever left --
     a task that once came within a few dozen bytes of its limit will eventually
     go past it, and that shows up here long before it crashes. */
  /* CPU time per task: microseconds since the start (run_time: of all, the
     time since the start once per core); the share over a while is the
     difference of two looks. */
  const UBaseType_t count = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> tasks(count);
  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t got = uxTaskGetSystemState(tasks.data(), count, &total);
  snprintf(buf, sizeof(buf), "\"run_time\":%llu,\"tasks\":[", static_cast<unsigned long long>(total));
  o += buf;
  for (UBaseType_t i = 0; i < got; i++) {
    /* Task names are compile-time literals today, so this escaping guards
       against a future one rather than a present hazard -- but it was the only
       string in the firmware reaching JSON unescaped, and the page puts it into
       the document. */
    char name[configMAX_TASK_NAME_LEN * 2];
    size_t w = 0;
    for (const char *c = tasks[i].pcTaskName; c && *c && w + 2 < sizeof(name); c++) {
      if (*c == '"' || *c == '\\')
        name[w++] = '\\';
      else if (static_cast<unsigned char>(*c) < 0x20)
        continue;
      name[w++] = *c;
    }
    name[w] = '\0';
    // the core: -1 for a task on either (tskNO_AFFINITY); the id tells tasks of one name apart (the http and the
    // https server are both "httpd")
    const int core = tasks[i].xCoreID == tskNO_AFFINITY ? -1 : static_cast<int>(tasks[i].xCoreID);
    snprintf(buf, sizeof(buf),
             "%s{\"id\":%u,\"name\":\"%s\",\"stack_free\":%u,\"prio\":%u,\"core\":%d,\"run_time\":%llu}",
             i ? "," : "", static_cast<unsigned>(tasks[i].xTaskNumber), name,
             static_cast<unsigned>(tasks[i].usStackHighWaterMark),
             static_cast<unsigned>(tasks[i].uxCurrentPriority), core,
             static_cast<unsigned long long>(tasks[i].ulRunTimeCounter));
    o += buf;
  }

  o += "]}";
  return o;
}

void check_stacks() {
  constexpr unsigned kWarnBelow = 1024, kStep = 256;
  static std::vector<std::pair<std::string, unsigned>> warned;   // task, the headroom last warned about
  const UBaseType_t count = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> tasks(count);
  const UBaseType_t got = uxTaskGetSystemState(tasks.data(), count, nullptr);
  for (UBaseType_t i = 0; i < got; i++) {
    const std::string name = tasks[i].pcTaskName ? tasks[i].pcTaskName : "";
    if (name.rfind("IDLE", 0) == 0 || name.rfind("ipc", 0) == 0 || name == "sys_evt")
      continue;
    const unsigned left = tasks[i].usStackHighWaterMark;
    if (left >= kWarnBelow)
      continue;
    auto it = std::find_if(warned.begin(), warned.end(), [&](const auto &w) { return w.first == name; });
    if (it != warned.end() && left + kStep > it->second)
      continue;
    ESP_LOGW(TAG, "Task \"%s\": only %u bytes of its stack were ever left -- give it more", name.c_str(), left);
    if (it == warned.end())
      warned.emplace_back(name, left);
    else
      it->second = left;
  }
}

}  // namespace diagnostics
