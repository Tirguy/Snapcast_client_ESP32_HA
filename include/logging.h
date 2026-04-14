#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

namespace snapcast {

enum class LogLevel : uint8_t {
  Error = 1,
  Info = 2,
  Warning = 3,
  Debug = 4,
};

#ifndef APP_LOG_LEVEL
#define APP_LOG_LEVEL 4
#endif

inline void log_message(LogLevel level, const char *tag, const char *fmt, ...) {
  if (static_cast<uint8_t>(level) > APP_LOG_LEVEL) {
    return;
  }

  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  const char *level_text = "INFO";
  if (level == LogLevel::Error) {
    level_text = "ERROR";
  } else if (level == LogLevel::Warning) {
    level_text = "WARN";
  } else if (level == LogLevel::Debug) {
    level_text = "DEBUG";
  }

  Serial.printf("[%10lu] [%s] [%s] %s\n", millis(), level_text, tag, message);
}

}  // namespace snapcast

#define LOGE(tag, fmt, ...) \
  ::snapcast::log_message(::snapcast::LogLevel::Error, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) \
  ::snapcast::log_message(::snapcast::LogLevel::Info, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) \
  ::snapcast::log_message(::snapcast::LogLevel::Warning, tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) \
  ::snapcast::log_message(::snapcast::LogLevel::Debug, tag, fmt, ##__VA_ARGS__)
