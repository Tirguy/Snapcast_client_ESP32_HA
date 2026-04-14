#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace snapcast {

bool startHeartbeatTask(uint32_t period_ms, TaskHandle_t *handle);

}  // namespace snapcast
