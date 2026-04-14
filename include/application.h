#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio_manager.h"
#include "display_manager.h"
#include "metadata_manager.h"
#include "network_manager.h"
#include "storage_service.h"
#include "voice_manager.h"

namespace snapcast {

class Application {
 public:
  enum class RuntimeMode : uint8_t {
    kIdle = 0,
    kSnapcast,
    kAssistListening,
    kAssistTts,
  };

  static Application &instance();

  bool begin();
  void tick();
  void onServerSettings(uint32_t volume_percent, bool muted, int32_t latency_ms,
                        int32_t buffer_ms);
  void onCodecHeaderPcm(uint32_t sample_rate_hz, uint16_t bits_per_sample,
                        uint16_t channels);
  void onWireChunkPcm(const uint8_t *payload, size_t size, int64_t chunk_ts_us);
  void onSyncOffsetUpdated(int64_t offset_us);
  void onStreamTrimChanged(int16_t trim_ms);
  void onEqSettingsChanged(const RuntimeNetworkSettings &settings);
  void onWifiStateChanged(bool connected, const char *ip);
  void onSnapcastStateChanged(bool connected);
  bool isStreamPlaying() const;
  bool isStreamAudioActive(uint32_t recent_ms = 1500) const;
  void enterVoiceCaptureMode();
  void leaveVoiceCaptureMode();
  bool startVoicePlayback(uint32_t sample_rate_hz, uint16_t channels);
  void feedVoicePcm(const uint8_t *pcm, size_t size);
  bool waitForVoicePlaybackDrain(uint32_t timeout_ms, uint32_t settle_ms = 120);
  void stopVoicePlayback();
  StorageService &storage();

 private:
  bool initBoard();
  bool initServices();
  bool startTasks();
  void setRuntimeMode(RuntimeMode mode, const char *reason = nullptr);
  void updateStatusLed();

  // Shared state accessed from both the main/tick task and the voice task
  // (dual-core ESP32): must be std::atomic to guarantee cache coherency.
  std::atomic<bool> tts_playing_{false};
  std::atomic<bool> voice_capture_mode_{false};
  bool last_stream_playing_ = false;
  uint32_t drop_chunks_until_ms_ = 0;
  bool snapcast_format_valid_ = false;
  uint32_t snapcast_rate_hz_ = 44100;
  uint16_t snapcast_bits_per_sample_ = 16;
  uint16_t snapcast_channels_ = 2;
  volatile uint32_t last_stream_chunk_ms_ = 0;
  RuntimeMode runtime_mode_ = RuntimeMode::kIdle;
  TaskHandle_t heartbeat_task_ = nullptr;
  StorageService storage_;
  AudioManager audio_;
  DisplayManager display_;
  MetadataManager metadata_;
  NetworkManager network_;
  VoiceManager voice_;
};

}  // namespace snapcast
