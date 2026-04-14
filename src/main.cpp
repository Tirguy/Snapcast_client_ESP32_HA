#include <Arduino.h>

#include "app_config.h"
#include "app_entry.h"
#include "application.h"
#include "logging.h"

namespace {
constexpr const char *kTag = "main";
}

void appSetup() {
  Serial.begin(snapcast::kAppConfig.serial_baudrate);
  delay(200);

  LOGI(kTag, "Booting %s", snapcast::kAppConfig.device_name);

  if (!snapcast::Application::instance().begin()) {
    LOGE(kTag, "Application initialization failed");
  }
}

void appLoop() {
  snapcast::Application::instance().tick();
  delay(snapcast::kAppConfig.main_loop_delay_ms);
}
