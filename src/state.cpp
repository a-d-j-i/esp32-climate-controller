#include "state.h"
#include "config.h"
#include "console_session.h"
#include <stdarg.h>
#include <esp_system.h>
#include <time.h>

std::recursive_mutex g_lock;
Settings g_settings;
Status g_status;

static String s_log[EVENT_LOG_SIZE];
static size_t s_logNext = 0;
static size_t s_logCount = 0;

String timestamp() {
  char buf[32];
  time_t now = time(nullptr);
  if (now > 1700000000) {  // clock set by network time
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  } else {
    snprintf(buf, sizeof buf, "+%lus", (unsigned long)(millis() / 1000));
  }
  return buf;
}

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart command";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog (the firmware hung)";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

void logEvent(const char* fmt, ...) {
  char msg[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  for (char* p = msg; *p; p++)
    if ((uint8_t)*p < 0x20 || *p == 0x7f) *p = '?';
  String line = timestamp() + " " + msg;
  consoleBroadcast(line);
  Lock lk(g_lock);
  s_log[s_logNext] = line;
  s_logNext = (s_logNext + 1) % EVENT_LOG_SIZE;
  if (s_logCount < EVENT_LOG_SIZE) s_logCount++;
}

std::vector<String> eventLogSnapshot() {
  Lock lk(g_lock);
  std::vector<String> out;
  size_t start = (s_logNext + EVENT_LOG_SIZE - s_logCount) % EVENT_LOG_SIZE;
  for (size_t i = 0; i < s_logCount; i++) out.push_back(s_log[(start + i) % EVENT_LOG_SIZE]);
  return out;
}
