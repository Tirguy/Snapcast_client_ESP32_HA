#pragma once

#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <array>
#include <stdint.h>
#include <stddef.h>

#include "storage_service.h"

namespace snapcast {

class AudioManager {
 public:
  struct BiquadCoeffs {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
  };

  struct BiquadState {
    float z1;
    float z2;
  };

  bool begin();
  void tick();
  void applyServerSettings(uint32_t volume_percent, bool muted, int32_t latency_ms,
                           int32_t buffer_ms);
  bool applyCodecPcmFormat(uint32_t sample_rate_hz, uint16_t bits_per_sample,
                           uint16_t channels);
  // chunk_ts_us: server-clock timestamp of this chunk (µs). Pass 0 if unknown.
  // blocking: if true, the call will wait up to 500ms for queue space (use for TTS to prevent drops).
  bool playPcmChunk(const uint8_t *payload, size_t size, int64_t chunk_ts_us = 0, bool blocking = false);
  // Force-stop current stream output: drop queued chunks and push silence to I2S.
  void stopStreamOutputNow();
  void suspendOutput();
  bool waitForPlaybackDrain(uint32_t timeout_ms, uint32_t settle_ms = 120);
  bool isOutputActive() const;
  // Update server-clock offset (server_clock - client_clock, µs).
  void setSyncOffset(int64_t offset_us);
  void setStreamTrimMs(int16_t trim_ms);
  void setEqSettings(EqPreset preset, int8_t bass_gain_db, uint16_t bass_freq_hz,
                     int8_t treble_gain_db, uint16_t treble_freq_hz);
  void flushQueuedChunks();

 private:
  struct PcmChunk {
    uint8_t *data;
    size_t size;
    int64_t server_ts_us;  // server-clock timestamp of this chunk
    int64_t duration_us;   // chunk duration derived from PCM format
  };

  bool configureOutput();
  bool reconfigureOutput();
  void updateEqFilters();
  void resetEqFilterState();
  bool startOutputTask();
  void outputTaskLoop();
  bool primeOutputPath();
  void applySoftwareVolume(uint8_t *buffer, size_t size) const;
  void applyEqualizer(int16_t *pcm_samples, size_t sample_count) const;
  bool writePcmChunkBlocking(const uint8_t *payload, size_t size);
  void playStartupTone();
  static void outputTaskEntry(void *context);

  bool i2s_started_ = false;
  uint32_t volume_percent_ = 100;
  bool muted_ = false;
  uint32_t sample_rate_hz_ = 44100;
  uint16_t bits_per_sample_ = 16;
  uint16_t channels_ = 2;
  int32_t buffer_ms_ = 1000;           // bufferMs from SERVER_SETTINGS
  volatile int16_t stream_trim_ms_ = 0;
  volatile int64_t server_offset_us_ = 0;  // server_clock - client_clock (µs)
  volatile bool sync_ready_ = false;
  volatile int64_t sync_scheduling_suspended_until_us_ = 0;
  uint8_t sync_overdue_streak_ = 0;
  uint8_t sync_suspend_hits_ = 0;
  bool sync_scheduling_disabled_ = false;
  EqPreset eq_preset_ = EqPreset::kNormal;
  int8_t eq_bass_gain_db_ = 0;
  int8_t eq_treble_gain_db_ = 0;
  uint16_t eq_bass_freq_hz_ = 120;
  uint16_t eq_treble_freq_hz_ = 6000;
  QueueHandle_t pcm_queue_ = nullptr;
  TaskHandle_t output_task_ = nullptr;
  uint32_t dropped_chunks_ = 0;
  i2s_chan_handle_t tx_channel_ = nullptr;
  volatile int64_t last_output_write_us_ = 0;
  uint32_t output_idle_ticks_ = 0;
  bool output_sleeping_ = false;

  mutable BiquadCoeffs low_shelf_coeffs_{};
  mutable BiquadCoeffs high_shelf_coeffs_{};
  mutable BiquadState low_shelf_state_[2]{};
  mutable BiquadState high_shelf_state_[2]{};
  static constexpr size_t kProcessingBufferSize = 4096;
  std::array<uint8_t, kProcessingBufferSize> processing_buffer_{};
  uint32_t last_process_us_ = 0;
};

}  // namespace snapcast
