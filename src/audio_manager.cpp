#include "audio_manager.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "app_config.h"
#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "audio";
constexpr float kPi = 3.14159265358979323846f;

bool hasPsram() {
  return ESP.getPsramSize() > 0;
}

template <typename T>
T clampValue(T value, T min_value, T max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

AudioManager::BiquadCoeffs designLowShelf(float sample_rate_hz, float freq_hz, float gain_db) {
  AudioManager::BiquadCoeffs coeffs{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  if (sample_rate_hz <= 0.0f || freq_hz <= 0.0f) {
    return coeffs;
  }

  const float nyquist = sample_rate_hz * 0.5f;
  const float f = clampValue(freq_hz, 20.0f, nyquist * 0.45f);
  const float A = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * kPi * (f / sample_rate_hz);
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float S = 1.0f;
  const float alpha = (sin_w0 * 0.5f) * sqrtf((A + (1.0f / A)) * ((1.0f / S) - 1.0f) + 2.0f);
  const float sqrtA = sqrtf(A);

  const float b0 = A * ((A + 1.0f) - ((A - 1.0f) * cos_w0) + (2.0f * sqrtA * alpha));
  const float b1 = 2.0f * A * ((A - 1.0f) - ((A + 1.0f) * cos_w0));
  const float b2 = A * ((A + 1.0f) - ((A - 1.0f) * cos_w0) - (2.0f * sqrtA * alpha));
  const float a0 = (A + 1.0f) + ((A - 1.0f) * cos_w0) + (2.0f * sqrtA * alpha);
  const float a1 = -2.0f * ((A - 1.0f) + ((A + 1.0f) * cos_w0));
  const float a2 = (A + 1.0f) + ((A - 1.0f) * cos_w0) - (2.0f * sqrtA * alpha);

  if (fabsf(a0) < 1e-9f) {
    return coeffs;
  }

  coeffs.b0 = b0 / a0;
  coeffs.b1 = b1 / a0;
  coeffs.b2 = b2 / a0;
  coeffs.a1 = a1 / a0;
  coeffs.a2 = a2 / a0;
  return coeffs;
}

AudioManager::BiquadCoeffs designHighShelf(float sample_rate_hz, float freq_hz, float gain_db) {
  AudioManager::BiquadCoeffs coeffs{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  if (sample_rate_hz <= 0.0f || freq_hz <= 0.0f) {
    return coeffs;
  }

  const float nyquist = sample_rate_hz * 0.5f;
  const float f = clampValue(freq_hz, 20.0f, nyquist * 0.45f);
  const float A = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * kPi * (f / sample_rate_hz);
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float S = 1.0f;
  const float alpha = (sin_w0 * 0.5f) * sqrtf((A + (1.0f / A)) * ((1.0f / S) - 1.0f) + 2.0f);
  const float sqrtA = sqrtf(A);

  const float b0 = A * ((A + 1.0f) + ((A - 1.0f) * cos_w0) + (2.0f * sqrtA * alpha));
  const float b1 = -2.0f * A * ((A - 1.0f) + ((A + 1.0f) * cos_w0));
  const float b2 = A * ((A + 1.0f) + ((A - 1.0f) * cos_w0) - (2.0f * sqrtA * alpha));
  const float a0 = (A + 1.0f) - ((A - 1.0f) * cos_w0) + (2.0f * sqrtA * alpha);
  const float a1 = 2.0f * ((A - 1.0f) - ((A + 1.0f) * cos_w0));
  const float a2 = (A + 1.0f) - ((A - 1.0f) * cos_w0) - (2.0f * sqrtA * alpha);

  if (fabsf(a0) < 1e-9f) {
    return coeffs;
  }

  coeffs.b0 = b0 / a0;
  coeffs.b1 = b1 / a0;
  coeffs.b2 = b2 / a0;
  coeffs.a1 = a1 / a0;
  coeffs.a2 = a2 / a0;
  return coeffs;
}

inline int16_t clampToInt16(float value) {
  if (value > 32767.0f) {
    return 32767;
  }
  if (value < -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(value);
}

uint8_t *allocatePcmChunk(size_t size) {
  if (hasPsram()) {
    if (auto *psram_ptr = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
      return psram_ptr;
    }
  }
  return static_cast<uint8_t *>(malloc(size));
}
}

bool AudioManager::begin() {
  sync_scheduling_disabled_ = !hasPsram();
  sync_scheduling_suspended_until_us_ = 0;
  sync_overdue_streak_ = 0;
  sync_suspend_hits_ = 0;
  if (sync_scheduling_disabled_) {
    LOGW(kTag,
         "PSRAM inactive: enabling stability mode (arrival-order playback, timestamp scheduling disabled)");
  }

  if (!configureOutput()) {
    return false;
  }

  if (!startOutputTask()) {
    return false;
  }

  if (kAppConfig.audio_runtime.play_startup_tone) {
    playStartupTone();
  }

  return true;
}

void AudioManager::tick() {}

void AudioManager::applyServerSettings(uint32_t volume_percent, bool muted,
                                       int32_t latency_ms, int32_t buffer_ms) {
  if (volume_percent > 100) {
    volume_percent = 100;
  }

  const bool changed = (volume_percent_ != volume_percent) || (muted_ != muted);
  volume_percent_ = volume_percent;
  muted_ = muted;
  if (buffer_ms > 0) {
    buffer_ms_ = buffer_ms;
  }

  if (changed) {
    LOGI(kTag,
         "Applied server settings: volume=%u muted=%s latency=%ldms buffer=%ldms",
         static_cast<unsigned>(volume_percent_), muted_ ? "true" : "false",
         static_cast<long>(latency_ms), static_cast<long>(buffer_ms_));
  }
}

void AudioManager::setSyncOffset(int64_t offset_us) {
  server_offset_us_ = offset_us;
  sync_ready_ = true;
}

void AudioManager::setStreamTrimMs(int16_t trim_ms) {
  stream_trim_ms_ = trim_ms;
  LOGI(kTag, "Runtime stream trim updated: %+d ms", static_cast<int>(trim_ms));
}

void AudioManager::setEqSettings(EqPreset preset, int8_t bass_gain_db,
                                 uint16_t bass_freq_hz, int8_t treble_gain_db,
                                 uint16_t treble_freq_hz) {
  eq_preset_ = preset;
  eq_bass_gain_db_ = clampValue<int8_t>(bass_gain_db, -12, 12);
  eq_treble_gain_db_ = clampValue<int8_t>(treble_gain_db, -12, 12);
  eq_bass_freq_hz_ = clampValue<uint16_t>(bass_freq_hz, 60, 400);
  eq_treble_freq_hz_ = clampValue<uint16_t>(treble_freq_hz, 2000, 14000);
  updateEqFilters();

  const char *preset_name = "Unknown";
  switch (preset) {
    case EqPreset::kNormal:      preset_name = "Normal"; break;
    case EqPreset::kBassBoost:   preset_name = "BassBoost"; break;
    case EqPreset::kTrebleBoost: preset_name = "TrebleBoost"; break;
    case EqPreset::kBright:      preset_name = "Bright"; break;
    case EqPreset::kCustom:      preset_name = "Custom"; break;
  }
  LOGI(kTag,
       "EQ updated: preset=%s bass=%d dB @ %u Hz treble=%d dB @ %u Hz",
       preset_name, static_cast<int>(eq_bass_gain_db_),
       static_cast<unsigned>(eq_bass_freq_hz_), static_cast<int>(eq_treble_gain_db_),
       static_cast<unsigned>(eq_treble_freq_hz_));
}

bool AudioManager::configureOutput() {
  updateEqFilters();
  return reconfigureOutput();
}

void AudioManager::resetEqFilterState() {
  memset(low_shelf_state_, 0, sizeof(low_shelf_state_));
  memset(high_shelf_state_, 0, sizeof(high_shelf_state_));
}

void AudioManager::updateEqFilters() {
  low_shelf_coeffs_ =
      designLowShelf(static_cast<float>(sample_rate_hz_), static_cast<float>(eq_bass_freq_hz_),
                     static_cast<float>(eq_bass_gain_db_));
  high_shelf_coeffs_ =
      designHighShelf(static_cast<float>(sample_rate_hz_), static_cast<float>(eq_treble_freq_hz_),
                      static_cast<float>(eq_treble_gain_db_));
  resetEqFilterState();
}

bool AudioManager::applyCodecPcmFormat(uint32_t sample_rate_hz,
                                       uint16_t bits_per_sample,
                                       uint16_t channels) {
  if (bits_per_sample != 16) {
    LOGE(kTag, "Unsupported PCM bits per sample: %u", static_cast<unsigned>(bits_per_sample));
    return false;
  }

  if (channels != 1 && channels != 2) {
    LOGE(kTag, "Unsupported PCM channel count: %u", static_cast<unsigned>(channels));
    return false;
  }

  sample_rate_hz_ = sample_rate_hz;
  bits_per_sample_ = bits_per_sample;
  channels_ = channels;
  updateEqFilters();

  flushQueuedChunks();

  if (!reconfigureOutput()) {
    return false;
  }

  // Brief stabilization delay: allows outputTaskLoop to switch to new I2S channel config
  // before feedVoicePcm() starts enqueueing frames
  vTaskDelay(pdMS_TO_TICKS(20));

  LOGI(kTag, "PCM format applied: %lu:%u:%u", static_cast<unsigned long>(sample_rate_hz_),
       static_cast<unsigned>(bits_per_sample_), static_cast<unsigned>(channels_));
  return true;
}

bool AudioManager::playPcmChunk(const uint8_t *payload, size_t size, int64_t chunk_ts_us, bool blocking) {
  if (!i2s_started_ || !pcm_queue_ || !payload || size == 0) {
    return false;
  }

  if ((size % 4) != 0) {
    LOGE(kTag, "PCM payload size is not aligned to 32-bit words: %u",
         static_cast<unsigned>(size));
    return false;
  }

  if (size > kAppConfig.audio_runtime.pcm_chunk_max_size) {
    LOGE(kTag, "PCM payload too large for queue: %u > %u", static_cast<unsigned>(size),
         static_cast<unsigned>(kAppConfig.audio_runtime.pcm_chunk_max_size));
    return false;
  }

  uint8_t *copy = allocatePcmChunk(size);
  if (!copy) {
    LOGE(kTag, "Failed to allocate PCM chunk copy (%u bytes)", static_cast<unsigned>(size));
    return false;
  }

  memcpy(copy, payload, size);
  const size_t frame_bytes = (bits_per_sample_ / 8) * channels_;
  const int64_t duration_us =
      (frame_bytes == 0 || sample_rate_hz_ == 0)
          ? 0
          : (static_cast<int64_t>(size / frame_bytes) * 1000000LL) /
                static_cast<int64_t>(sample_rate_hz_);
  PcmChunk chunk = {copy, size, chunk_ts_us, duration_us};

  if (blocking) {
    // For TTS: wait up to 500ms for queue space instead of dropping.
    // This throttles the MP3 decoder to the actual audio playback rate.
    if (xQueueSend(pcm_queue_, &chunk, pdMS_TO_TICKS(500)) == pdPASS) {
      return true;
    }
    // Timeout: queue stuck (possible I2S issue), give up to avoid stall.
    free(copy);
    LOGW(kTag, "TTS PCM blocking send timed out, chunk dropped");
    return false;
  }

  // Non-blocking path (Snapcast live stream): drop oldest on overflow.
  if (xQueueSend(pcm_queue_, &chunk, 0) == pdPASS) {
    return true;
  }

  PcmChunk stale = {nullptr, 0, 0, 0};
  if (xQueueReceive(pcm_queue_, &stale, 0) == pdPASS && stale.data) {
    free(stale.data);
  }

  if (xQueueSend(pcm_queue_, &chunk, 0) == pdPASS) {
    ++dropped_chunks_;
    if ((dropped_chunks_ % 25) == 1) {
      LOGW(kTag, "PCM queue overflow, dropping oldest chunk (count=%lu)",
           static_cast<unsigned long>(dropped_chunks_));
    }
    return true;
  }

  free(copy);
  ++dropped_chunks_;
  if ((dropped_chunks_ % 25) == 1) {
    LOGW(kTag, "PCM queue overflow, dropping newest chunk (count=%lu)",
         static_cast<unsigned long>(dropped_chunks_));
  }
  return false;
}

void AudioManager::stopStreamOutputNow() {
  flushQueuedChunks();
  // Prime with silence to clear residual DAC/I2S content after abrupt stream stop.
  if (!primeOutputPath()) {
    LOGW(kTag, "Failed to push silence on stream stop");
  }
}

void AudioManager::suspendOutput() {
  flushQueuedChunks();
  i2s_started_ = false;
  last_output_write_us_ = 0;
  output_idle_ticks_ = 0;
  output_sleeping_ = false;

  if (tx_channel_) {
    if (!output_sleeping_) {
      i2s_channel_disable(tx_channel_);
    }
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
    LOGI(kTag, "I2S output suspended and GPIO released");
  }
}

bool AudioManager::waitForPlaybackDrain(uint32_t timeout_ms, uint32_t settle_ms) {
  if (!pcm_queue_) {
    return true;
  }

  const uint32_t start_ms = millis();
  while ((millis() - start_ms) < timeout_ms) {
    const UBaseType_t queued = uxQueueMessagesWaiting(pcm_queue_);
    const int64_t last_write_us = last_output_write_us_;
    const bool settle_done =
        (last_write_us == 0) ||
        ((esp_timer_get_time() - last_write_us) >= static_cast<int64_t>(settle_ms) * 1000LL);
    if (queued == 0 && settle_done) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  LOGW(kTag, "Timed out while waiting for playback drain");
  return false;
}

bool AudioManager::isOutputActive() const { return i2s_started_ && tx_channel_ != nullptr; }

bool AudioManager::startOutputTask() {
  if (pcm_queue_ == nullptr) {
    pcm_queue_ = xQueueCreate(kAppConfig.audio_runtime.pcm_queue_depth, sizeof(PcmChunk));
  }

  if (pcm_queue_ == nullptr) {
    LOGE(kTag, "Failed to create PCM queue");
    return false;
  }

  if (output_task_ != nullptr) {
    return true;
  }

  const BaseType_t task_ok = xTaskCreatePinnedToCore(outputTaskEntry, "pcm_output", 4096, this,
                                                     2, &output_task_, 1);
  if (task_ok != pdPASS) {
    LOGE(kTag, "Failed to create PCM output task");
    return false;
  }

  return true;
}

void AudioManager::outputTaskEntry(void *context) {
  static_cast<AudioManager *>(context)->outputTaskLoop();
}

void AudioManager::outputTaskLoop() {
  constexpr uint32_t kQueuePollMs = 20;
  constexpr uint32_t kIdleSleepAfterMs = 4000;
  constexpr uint32_t kIdleSleepTicksThreshold = kIdleSleepAfterMs / kQueuePollMs;
  static int32_t last_logged_effective_buffer_ms = -1;
  static bool logged_trim = false;
  static uint64_t bench_processing_us = 0;
  static uint64_t bench_audio_us = 0;
  static uint32_t bench_chunks = 0;
  static uint32_t bench_last_log_ms = 0;
  static uint8_t silence_idle[1024] = {0};

  while (true) {
    PcmChunk chunk = {nullptr, 0, 0, 0};
    if (xQueueReceive(pcm_queue_, &chunk, pdMS_TO_TICKS(kQueuePollMs)) != pdPASS) {
      // Keep I2S fed with silence when queue is empty to avoid DAC/driver noise on underrun.
      if (i2s_started_ && tx_channel_) {
        if (!output_sleeping_) {
          output_idle_ticks_++;
          if (output_idle_ticks_ > kIdleSleepTicksThreshold) {
            LOGI(kTag, "Audio idle timeout, disabling I2S channel to prevent noise");
            i2s_channel_disable(tx_channel_);
            output_sleeping_ = true;
          } else {
            size_t written = 0;
            i2s_channel_write(tx_channel_, silence_idle, sizeof(silence_idle), &written,
                              pdMS_TO_TICKS(5));
          }
        }
      }
      continue;
    }

    if (!chunk.data) {
      continue;
    }

    if (output_sleeping_ && i2s_started_ && tx_channel_) {
      LOGI(kTag, "Audio resume, enabling I2S channel");
      if (i2s_channel_enable(tx_channel_) != ESP_OK) {
        LOGW(kTag, "I2S channel enable skipped/failed during resume");
      }
      // Push a short silence burst on resume to re-prime the DMA path cleanly.
      if (!primeOutputPath()) {
        LOGW(kTag, "I2S prime skipped on resume");
      }
      output_sleeping_ = false;
    }
    output_idle_ticks_ = 0;

    // Timestamp-based scheduling when sync is established and the chunk carries
    // a valid server timestamp. If repeated overdue events happen, temporarily
    // bypass timestamp scheduling and play chunks in arrival order to recover.
    const int64_t now_us_for_sync_gate = esp_timer_get_time();
    const bool sync_scheduling_active =
      !sync_scheduling_disabled_ &&
      ((sync_scheduling_suspended_until_us_ == 0) ||
       (now_us_for_sync_gate >= sync_scheduling_suspended_until_us_));
    if (sync_ready_ && sync_scheduling_active && chunk.server_ts_us > 0) {
      if (!logged_trim) {
        logged_trim = true;
        LOGI(kTag, "Sync trim: %+ld ms", static_cast<long>(kAppConfig.audio_runtime.sync_trim_ms));
      }
      int64_t effective_buffer_us = static_cast<int64_t>(buffer_ms_) * 1000LL;
      if (chunk.duration_us > 0) {
        constexpr uint8_t kQueueHeadroomChunks = 2;
        const uint8_t queue_depth = kAppConfig.audio_runtime.pcm_queue_depth;
        const uint8_t usable_chunks =
            (queue_depth > kQueueHeadroomChunks) ? (queue_depth - kQueueHeadroomChunks) : 1;
        const int64_t queue_safe_buffer_us = chunk.duration_us * static_cast<int64_t>(usable_chunks);
        if (effective_buffer_us > queue_safe_buffer_us) {
          effective_buffer_us = queue_safe_buffer_us;
        }
        const int32_t effective_buffer_ms = static_cast<int32_t>(effective_buffer_us / 1000LL);
        if (effective_buffer_ms != last_logged_effective_buffer_ms) {
          last_logged_effective_buffer_ms = effective_buffer_ms;
          LOGW(kTag,
               "Effective sync buffer capped to %ld ms by local PCM queue capacity (server asked %ld ms, depth=%u)",
               static_cast<long>(effective_buffer_ms), static_cast<long>(buffer_ms_),
               static_cast<unsigned>(queue_depth));
        }
      }

      // play_at_local = chunk_ts_server - offset + effective_buffer
      // offset = server_clock - client_clock  =>  local = server - offset
        const int64_t total_trim_us =
          (static_cast<int64_t>(kAppConfig.audio_runtime.sync_trim_ms) +
           static_cast<int64_t>(stream_trim_ms_)) *
          1000LL;
      const int64_t play_at_local_us =
          chunk.server_ts_us - server_offset_us_ +
          effective_buffer_us + total_trim_us;
      const int64_t now_us = esp_timer_get_time();
      const int64_t wait_us = play_at_local_us - now_us;
      const int64_t far_future_threshold_us =
          effective_buffer_us + 300000LL;

      if (wait_us > far_future_threshold_us) {
        // Chunk significantly beyond the configured Snapcast buffer window:
        // sync offset is likely stale or corrupted, so skip it.
        LOGW(kTag, "Chunk far in future (%lld ms), discarding",
             static_cast<long long>(wait_us / 1000));
        free(chunk.data);
        continue;
      }

      if (wait_us < -500000LL) {
        // Chunk more than 500 ms overdue: discard to recover sync without reacting
        // to normal short jitter from network/task scheduling.
        static int64_t last_log_discard = 0;
        uint32_t flushed = 0;
        PcmChunk stale = {nullptr, 0, 0, 0};
        while (xQueueReceive(pcm_queue_, &stale, 0) == pdPASS) {
          if (stale.data) {
            free(stale.data);
          }
          ++flushed;
        }
        if (now_us - last_log_discard > 1000000LL) {
          LOGW(kTag, "Chunk overdue (%lld ms), dropping + flushing %lu queued chunks",
               static_cast<long long>(-wait_us / 1000),
               static_cast<unsigned long>(flushed));
          last_log_discard = now_us;
        }

        if (sync_overdue_streak_ < 255) {
          ++sync_overdue_streak_;
        }
        if (sync_overdue_streak_ >= 3) {
          sync_scheduling_suspended_until_us_ = now_us + 5000000LL;
          sync_overdue_streak_ = 0;
          if (sync_suspend_hits_ < 255) {
            ++sync_suspend_hits_;
          }
          LOGW(kTag,
               "Sync scheduling temporarily disabled for 5000 ms after repeated overdue chunks (hits=%u)",
               static_cast<unsigned>(sync_suspend_hits_));

          if (!hasPsram() && sync_suspend_hits_ >= 3 && !sync_scheduling_disabled_) {
            sync_scheduling_disabled_ = true;
            sync_scheduling_suspended_until_us_ = 0;
            LOGW(kTag,
                 "Sync scheduling permanently disabled (no PSRAM, repeated instability)");
          }
        }

        free(chunk.data);
        continue;
      }

      sync_overdue_streak_ = 0;

      if (wait_us > 1000LL) {
        // Sleep for the coarse part (1-ms granularity)
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        // Busy-spin for the last millisecond for sub-ms accuracy
        const int64_t fine_deadline = play_at_local_us;
        while (esp_timer_get_time() < fine_deadline) {
          // intentional busy spin ≤ 1 ms
        }
      }
    } else if (sync_ready_ && !sync_scheduling_active && chunk.server_ts_us > 0) {
      static int64_t last_log_suspended = 0;
      if (now_us_for_sync_gate - last_log_suspended > 1000000LL) {
        if (sync_scheduling_disabled_) {
          LOGW(kTag,
               "Sync scheduling disabled, playing by arrival order");
        } else {
          const int64_t remaining_ms =
              (sync_scheduling_suspended_until_us_ > now_us_for_sync_gate)
                  ? (sync_scheduling_suspended_until_us_ - now_us_for_sync_gate) / 1000LL
                  : 0;
          LOGW(kTag,
               "Sync scheduling suspended, playing by arrival order (remaining=%lld ms)",
               static_cast<long long>(remaining_ms));
        }
        last_log_suspended = now_us_for_sync_gate;
      }
    }

    if (!muted_) {
      writePcmChunkBlocking(chunk.data, chunk.size);
      bench_processing_us += last_process_us_;
      if (chunk.duration_us > 0) {
        bench_audio_us += static_cast<uint64_t>(chunk.duration_us);
      }
      ++bench_chunks;

      const uint32_t now_ms = millis();
      if (bench_last_log_ms == 0) {
        bench_last_log_ms = now_ms;
      }
      if ((now_ms - bench_last_log_ms) >= 5000 && bench_chunks > 0) {
        const uint32_t cpu_est =
            (bench_audio_us > 0)
                ? static_cast<uint32_t>((bench_processing_us * 100ULL) / bench_audio_us)
                : 0;
        LOGI(kTag,
             "EQ benchmark: chunks=%lu proc_us=%llu audio_us=%llu est_cpu=%lu%% heap=%lu psram_free=%lu",
             static_cast<unsigned long>(bench_chunks),
             static_cast<unsigned long long>(bench_processing_us),
             static_cast<unsigned long long>(bench_audio_us),
             static_cast<unsigned long>(cpu_est),
             static_cast<unsigned long>(ESP.getFreeHeap()),
             static_cast<unsigned long>(ESP.getFreePsram()));
        bench_processing_us = 0;
        bench_audio_us = 0;
        bench_chunks = 0;
        bench_last_log_ms = now_ms;
      }
    }
    last_output_write_us_ = esp_timer_get_time();
    free(chunk.data);
  }
}

bool AudioManager::writePcmChunkBlocking(const uint8_t *payload, size_t size) {
  if (!i2s_started_ || !tx_channel_ || !payload || size == 0) {
    return false;
  }

  uint32_t process_start_us = static_cast<uint32_t>(micros());
  const bool eq_active =
      (eq_bass_gain_db_ != 0) || (eq_treble_gain_db_ != 0);
  const bool processing_required =
      kAppConfig.audio_runtime.swap_pcm_frame_words || (volume_percent_ < 100) || eq_active;

  const uint8_t *output_ptr = payload;
  if (processing_required) {
    memcpy(processing_buffer_.data(), payload, size);
    output_ptr = processing_buffer_.data();

    if (kAppConfig.audio_runtime.swap_pcm_frame_words) {
      uint8_t *swapped = processing_buffer_.data();
      for (size_t i = 0; i < size; i += 4) {
        const uint8_t b0 = swapped[i + 0];
        const uint8_t b1 = swapped[i + 1];
        swapped[i + 0] = swapped[i + 2];
        swapped[i + 1] = swapped[i + 3];
        swapped[i + 2] = b0;
        swapped[i + 3] = b1;
      }
    }

    if (volume_percent_ < 100) {
      applySoftwareVolume(processing_buffer_.data(), size);
    }

    if (eq_active) {
      applyEqualizer(reinterpret_cast<int16_t *>(processing_buffer_.data()),
                     size / sizeof(int16_t));
    }
  }
  last_process_us_ = static_cast<uint32_t>(micros()) - process_start_us;

  size_t written = 0;
  const esp_err_t err = i2s_channel_write(tx_channel_, output_ptr, size, &written,
                                          pdMS_TO_TICKS(250));
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_write failed for PCM chunk: %d", err);
    return false;
  }

  if (written != size) {
    LOGW(kTag, "Short I2S write: %u/%u bytes", static_cast<unsigned>(written),
         static_cast<unsigned>(size));
  }

  return true;
}

void AudioManager::applySoftwareVolume(uint8_t *buffer, size_t size) const {
  if (!buffer || size < sizeof(int16_t)) {
    return;
  }

  if (volume_percent_ >= 100) {
    return;
  }

  if (volume_percent_ == 0) {
    memset(buffer, 0, size);
    return;
  }

  const int32_t gain = static_cast<int32_t>(volume_percent_);
  int16_t *samples = reinterpret_cast<int16_t *>(buffer);
  const size_t sample_count = size / sizeof(int16_t);
  for (size_t i = 0; i < sample_count; ++i) {
    int32_t scaled = (static_cast<int32_t>(samples[i]) * gain) / 100;
    if (scaled > 32767) {
      scaled = 32767;
    } else if (scaled < -32768) {
      scaled = -32768;
    }
    samples[i] = static_cast<int16_t>(scaled);
  }
}

void AudioManager::applyEqualizer(int16_t *pcm_samples, size_t sample_count) const {
  if (!pcm_samples || sample_count == 0) {
    return;
  }

  const size_t channel_count = (channels_ == 1) ? 1 : 2;
  const size_t frame_count = sample_count / channel_count;
  for (size_t frame = 0; frame < frame_count; ++frame) {
    for (size_t ch = 0; ch < channel_count; ++ch) {
      const size_t idx = frame * channel_count + ch;
      float x = static_cast<float>(pcm_samples[idx]);

      float y_low = (low_shelf_coeffs_.b0 * x) + low_shelf_state_[ch].z1;
      low_shelf_state_[ch].z1 =
          (low_shelf_coeffs_.b1 * x) - (low_shelf_coeffs_.a1 * y_low) + low_shelf_state_[ch].z2;
      low_shelf_state_[ch].z2 =
          (low_shelf_coeffs_.b2 * x) - (low_shelf_coeffs_.a2 * y_low);

      float y_high = (high_shelf_coeffs_.b0 * y_low) + high_shelf_state_[ch].z1;
      high_shelf_state_[ch].z1 = (high_shelf_coeffs_.b1 * y_low) -
                                 (high_shelf_coeffs_.a1 * y_high) +
                                 high_shelf_state_[ch].z2;
      high_shelf_state_[ch].z2 =
          (high_shelf_coeffs_.b2 * y_low) - (high_shelf_coeffs_.a2 * y_high);

      pcm_samples[idx] = clampToInt16(y_high);
    }
  }
}

bool AudioManager::reconfigureOutput() {
  last_output_write_us_ = 0;
  i2s_started_ = false;

  if (tx_channel_) {
    if (!output_sleeping_) {
      i2s_channel_disable(tx_channel_);
    }
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
  }

  output_idle_ticks_ = 0;
  output_sleeping_ = false;

  const i2s_chan_config_t channel_config = {
      .id = static_cast<i2s_port_t>(kAppConfig.audio.port),
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 8,
      .dma_frame_num = 256,
      .auto_clear = true,
  };

  esp_err_t err = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_new_channel failed: %d", err);
    return false;
  }

  i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz_);
  clock_config.clk_src = I2S_CLK_SRC_APLL;

  const i2s_data_bit_width_t bit_width =
      (bits_per_sample_ == 16) ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT;
  const i2s_slot_mode_t slot_mode = (channels_ == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

  i2s_std_config_t std_config = {
      .clk_cfg = clock_config,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(kAppConfig.audio.bclk),
          .ws = static_cast<gpio_num_t>(kAppConfig.audio.ws),
          .dout = static_cast<gpio_num_t>(kAppConfig.audio.data_out),
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };

  err = i2s_channel_init_std_mode(tx_channel_, &std_config);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_channel_init_std_mode failed: %d", err);
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
    return false;
  }

  err = i2s_channel_enable(tx_channel_);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_channel_enable failed: %d", err);
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
    return false;
  }

  if (!primeOutputPath()) {
    LOGW(kTag, "I2S prime skipped");
  }

  i2s_started_ = true;
  LOGI(kTag, "I2S std output ready sr=%lu bits=%u ch=%u BCLK=%d WS=%d DOUT=%d",
       static_cast<unsigned long>(sample_rate_hz_), static_cast<unsigned>(bits_per_sample_),
       static_cast<unsigned>(channels_), kAppConfig.audio.bclk, kAppConfig.audio.ws,
       kAppConfig.audio.data_out);
  return true;
}

bool AudioManager::primeOutputPath() {
  if (!tx_channel_) {
    return false;
  }

  static uint8_t silence[1024] = {0};
  size_t written = 0;
  const esp_err_t err =
      i2s_channel_write(tx_channel_, silence, sizeof(silence), &written, pdMS_TO_TICKS(100));
  return err == ESP_OK;
}

void AudioManager::flushQueuedChunks() {
  if (!pcm_queue_) {
    return;
  }

  PcmChunk chunk = {nullptr, 0, 0, 0};
  while (xQueueReceive(pcm_queue_, &chunk, 0) == pdPASS) {
    if (chunk.data) {
      free(chunk.data);
    }
  }
}

void AudioManager::playStartupTone() {
  if (!tx_channel_) {
    return;
  }

  constexpr size_t kFrameSamples = 256;
  int16_t pcm[kFrameSamples * 2];

  const float sr = static_cast<float>(kAppConfig.audio_runtime.sample_rate_hz);
  const float freq = static_cast<float>(kAppConfig.audio_runtime.tone_frequency_hz);
  const float phase_step = 2.0f * static_cast<float>(M_PI) * (freq / sr);
  float phase = 0.0f;

  const uint32_t total_samples =
      (kAppConfig.audio_runtime.sample_rate_hz * kAppConfig.audio_runtime.tone_duration_ms) /
      1000;

  uint32_t generated = 0;
  while (generated < total_samples) {
    const size_t this_block =
        (total_samples - generated > kFrameSamples) ? kFrameSamples : (total_samples - generated);

    for (size_t i = 0; i < this_block; ++i) {
      const int16_t sample = static_cast<int16_t>(
          kAppConfig.audio_runtime.tone_amplitude * sinf(phase));
      pcm[i * 2] = sample;
      pcm[i * 2 + 1] = sample;
      phase += phase_step;
      if (phase >= 2.0f * static_cast<float>(M_PI)) {
        phase -= 2.0f * static_cast<float>(M_PI);
      }
    }

    size_t written = 0;
    const size_t bytes = this_block * 2 * sizeof(int16_t);
    const esp_err_t err = i2s_channel_write(tx_channel_, pcm, bytes, &written,
                                            pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
      LOGE(kTag, "i2s_write failed during startup tone: %d", err);
      break;
    }

    generated += this_block;
  }

  LOGI(kTag, "Startup tone played (%u ms @ %u Hz)", kAppConfig.audio_runtime.tone_duration_ms,
       kAppConfig.audio_runtime.tone_frequency_hz);
}

}  // namespace snapcast
