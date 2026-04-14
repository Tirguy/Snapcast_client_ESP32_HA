#include "heartbeat_task.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "../logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "heartbeat";

struct HeartbeatContext {
  uint32_t period_ms;
};

void heartbeatTask(void *param) {
  const HeartbeatContext context = *static_cast<HeartbeatContext *>(param);
  delete static_cast<HeartbeatContext *>(param);

  while (true) {
    const uint32_t free_heap = ESP.getFreeHeap();
    const uint32_t free_psram = ESP.getFreePsram();
    const size_t dma_heap = heap_caps_get_free_size(MALLOC_CAP_DMA);

    LOGD(kTag, "alive | heap=%lu | psram=%lu | dma=%u",
         static_cast<unsigned long>(free_heap),
         static_cast<unsigned long>(free_psram), static_cast<unsigned int>(dma_heap));

    vTaskDelay(pdMS_TO_TICKS(context.period_ms));
  }
}
}  // namespace

bool startHeartbeatTask(uint32_t period_ms, TaskHandle_t *handle) {
  auto *context = new HeartbeatContext{period_ms};
  const BaseType_t result = xTaskCreate(heartbeatTask, "heartbeat", 4096, context, 1, handle);
  if (result != pdPASS) {
    delete context;
    LOGE(kTag, "Failed to create heartbeat task");
    return false;
  }

  LOGI(kTag, "Heartbeat task started");
  return true;
}

}  // namespace snapcast