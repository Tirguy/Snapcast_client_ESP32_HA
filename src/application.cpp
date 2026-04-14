#include "application.h"

#include <Arduino.h>

#include "SPIFFS.h"
#include "app_config.h"
#include "logging.h"
#include "tasks/heartbeat_task.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "app";
constexpr uint32_t kDropChunksAfterStopMs = 2500;

const char *runtimeModeToString(Application::RuntimeMode mode) {
  switch (mode) {
    case Application::RuntimeMode::kIdle:
      return "IDLE";
    case Application::RuntimeMode::kSnapcast:
      return "SNAPCAST";
    case Application::RuntimeMode::kAssistListening:
      return "ASSIST_LISTENING";
    case Application::RuntimeMode::kAssistTts:
      return "ASSIST_TTS";
  }
  return "UNKNOWN";
}
}

Application &Application::instance() {
  static Application app;
  return app;
}

bool Application::begin() {
  LOGI(kTag, "Starting application skeleton");
  return initBoard() && initServices() && startTasks();
}

void Application::tick() {
  const RuntimeNetworkSettings &settings = storage_.networkSettings();
  metadata_.setTarget(settings.snapcast_host, settings.snapcast_port);
  metadata_.tick();

  const bool stream_playing = metadata_.isPlaying();
  const bool stream_recent = isStreamAudioActive();

  if (stream_playing && voice_capture_mode_) {
    // MA stream has priority: stop any ongoing voice-capture intent.
    voice_capture_mode_ = false;
    LOGW(kTag, "Stream playing detected, forcing voice capture OFF");
  }

  if (last_stream_playing_ && !stream_playing && !tts_playing_) {
    audio_.stopStreamOutputNow();
    drop_chunks_until_ms_ = millis() + kDropChunksAfterStopMs;
  }
  last_stream_playing_ = stream_playing;

  if (tts_playing_) {
    setRuntimeMode(RuntimeMode::kAssistTts, "tts_playing");
  } else if (stream_playing || stream_recent) {
    setRuntimeMode(RuntimeMode::kSnapcast, stream_playing ? "ma_playing" : "stream_recent");
  } else if (voice_capture_mode_) {
    setRuntimeMode(RuntimeMode::kAssistListening, "voice_capture");
  } else {
    setRuntimeMode(RuntimeMode::kIdle, "no_stream_no_voice");
  }

  display_.setNowPlaying(metadata_.title().c_str(), metadata_.artist().c_str(),
                        metadata_.album().c_str(), stream_playing);
  display_.tick();

  audio_.tick();
  network_.tick();
  voice_.tick();
}

void Application::onServerSettings(uint32_t volume_percent, bool muted,
                                   int32_t latency_ms, int32_t buffer_ms) {
  audio_.applyServerSettings(volume_percent, muted, latency_ms, buffer_ms);
  display_.setVolumeState(volume_percent, muted);
}

void Application::onCodecHeaderPcm(uint32_t sample_rate_hz,
                                   uint16_t bits_per_sample,
                                   uint16_t channels) {
  snapcast_format_valid_ = true;
  snapcast_rate_hz_ = sample_rate_hz;
  snapcast_bits_per_sample_ = bits_per_sample;
  snapcast_channels_ = channels;
  if (runtime_mode_ == RuntimeMode::kAssistTts) {
    return;
  }
  audio_.applyCodecPcmFormat(sample_rate_hz, bits_per_sample, channels);
}

void Application::onWireChunkPcm(const uint8_t *payload, size_t size,
                                 int64_t chunk_ts_us) {
  last_stream_chunk_ms_ = millis();
  if (runtime_mode_ == RuntimeMode::kAssistTts) {
    return;  // bloquer Snapcast pendant la lecture TTS
  }
  if (drop_chunks_until_ms_ != 0 &&
      static_cast<int32_t>(millis() - drop_chunks_until_ms_) < 0) {
    return;  // purge temporelle courte après stop, sans bloquer les reprises
  }
  audio_.playPcmChunk(payload, size, chunk_ts_us);
}

void Application::onSyncOffsetUpdated(int64_t offset_us) {
  audio_.setSyncOffset(offset_us);
}

void Application::onStreamTrimChanged(int16_t trim_ms) {
  audio_.setStreamTrimMs(trim_ms);
}

void Application::onEqSettingsChanged(const RuntimeNetworkSettings &settings) {
  audio_.setEqSettings(settings.eq_preset, settings.eq_bass_gain_db,
                       settings.eq_bass_freq_hz, settings.eq_treble_gain_db,
                       settings.eq_treble_freq_hz);
}

void Application::onWifiStateChanged(bool connected, const char *ip) {
  display_.setWifiState(connected, ip);
}

void Application::onSnapcastStateChanged(bool connected) {
  display_.setSnapcastState(connected);
}

bool Application::isStreamPlaying() const { return metadata_.isPlaying(); }

bool Application::isStreamAudioActive(uint32_t recent_ms) const {
  if (last_stream_chunk_ms_ == 0) {
    return false;
  }
  return static_cast<uint32_t>(millis() - last_stream_chunk_ms_) <= recent_ms;
}

void Application::enterVoiceCaptureMode() {
  if (isStreamPlaying()) {
    // Policy: if MA reports stream playing, microphone must stay off.
    voice_capture_mode_ = false;
    setRuntimeMode(RuntimeMode::kSnapcast, "voice_request_blocked_by_ma_playing");
    return;
  }
  voice_capture_mode_ = true;
  setRuntimeMode(RuntimeMode::kAssistListening, "voice_capture_enter");
}

void Application::leaveVoiceCaptureMode() {
  voice_capture_mode_ = false;
  if (tts_playing_) {
    setRuntimeMode(RuntimeMode::kAssistTts, "voice_capture_leave_tts_on");
  } else if (isStreamPlaying() || isStreamAudioActive()) {
    setRuntimeMode(RuntimeMode::kSnapcast, "voice_capture_leave_stream_present");
  } else {
    setRuntimeMode(RuntimeMode::kIdle, "voice_capture_leave_idle");
  }
}

bool Application::startVoicePlayback(uint32_t sample_rate_hz, uint16_t channels) {
  LOGI(kTag, "startVoicePlayback: sr=%lu ch=%u", static_cast<unsigned long>(sample_rate_hz), static_cast<unsigned>(channels));
  tts_playing_ = true;
  setRuntimeMode(RuntimeMode::kAssistTts, "tts_start");
  // Flush any Snapcast chunks still in the PCM queue before switching I2S format.
  // Without this, leftover 48kHz/stereo Snapcast chunks would be played through
  // the 24kHz/mono TTS I2S path, producing noise and delaying or masking TTS audio.
  audio_.flushQueuedChunks();
  bool ret = audio_.applyCodecPcmFormat(sample_rate_hz, 16, channels);
  LOGI(kTag, "startVoicePlayback ret=%s", ret ? "true" : "false");
  return ret;
}

void Application::feedVoicePcm(const uint8_t *pcm, size_t size) {
  // Ensure alignment to 4 bytes required by playPcmChunk
  size_t aligned = size & ~3u;
  if (aligned > 0) {
    // blocking=true: throttle the TTS decoder to audio playback speed (prevents PCM queue overflow)
    bool ret = audio_.playPcmChunk(pcm, aligned, 0, true);
    if (!ret) {
      LOGW(kTag, "feedVoicePcm: playPcmChunk FAILED size=%u", static_cast<unsigned>(aligned));
    }
  }
}

bool Application::waitForVoicePlaybackDrain(uint32_t timeout_ms, uint32_t settle_ms) {
  return audio_.waitForPlaybackDrain(timeout_ms, settle_ms);
}

void Application::stopVoicePlayback() {
  LOGI(kTag, "stopVoicePlayback: restoring sr=%lu bits=%u ch=%u",
       static_cast<unsigned long>(snapcast_rate_hz_),
       static_cast<unsigned>(snapcast_bits_per_sample_),
       static_cast<unsigned>(snapcast_channels_));
  tts_playing_ = false;
  if (!snapcast_format_valid_) {
    LOGW(kTag, "No valid Snapcast format cached after TTS, keep current output format");
    return;
  }
  // Restaurer le format I2S du stream Snapcast après lecture TTS
  audio_.applyCodecPcmFormat(snapcast_rate_hz_, snapcast_bits_per_sample_, snapcast_channels_);

  if (isStreamPlaying() || isStreamAudioActive()) {
    setRuntimeMode(RuntimeMode::kSnapcast, "tts_stop_stream_present");
  } else {
    setRuntimeMode(RuntimeMode::kIdle, "tts_stop_idle");
  }
}

void Application::setRuntimeMode(RuntimeMode mode, const char *reason) {
  if (runtime_mode_ == mode) {
    return;
  }
  if (reason && reason[0] != '\0') {
    LOGI(kTag, "Mode transition: %s -> %s (%s)",
         runtimeModeToString(runtime_mode_), runtimeModeToString(mode), reason);
  } else {
    LOGI(kTag, "Mode transition: %s -> %s", runtimeModeToString(runtime_mode_),
         runtimeModeToString(mode));
  }
  runtime_mode_ = mode;
  updateStatusLed();
}

void Application::updateStatusLed() {
  if (kAppConfig.status_led_gpio < 0) {
    return;
  }

  // Requirement: LED ON when there is no Snapcast audio stream, OFF while streaming.
  const bool stream_active = (runtime_mode_ == RuntimeMode::kSnapcast);
  digitalWrite(kAppConfig.status_led_gpio, stream_active ? LOW : HIGH);
}

StorageService &Application::storage() { return storage_; }

bool Application::initBoard() {
  LOGI(kTag, "Board config: I2S BCLK=%d WS=%d DOUT=%d DIN=%d", kAppConfig.audio.bclk,
       kAppConfig.audio.ws, kAppConfig.audio.data_out, kAppConfig.audio.data_in);
  LOGI(kTag, "Display config: CS=%d DC=%d RST=%d SCK=%d MOSI=%d", kAppConfig.display.cs,
       kAppConfig.display.dc, kAppConfig.display.reset, kAppConfig.display.sck,
       kAppConfig.display.mosi);

  if (kAppConfig.status_led_gpio >= 0) {
    pinMode(kAppConfig.status_led_gpio, OUTPUT);
    updateStatusLed();
    LOGI(kTag, "Status LED configured on GPIO%d (ON=no stream, OFF=stream)",
         static_cast<int>(kAppConfig.status_led_gpio));
  }
  return true;
}

bool Application::initServices() {
  if (!storage_.begin()) {
    LOGE(kTag, "SPIFFS initialization failed");
    return false;
  }

  const uint32_t boot_count = storage_.incrementBootCount();
  LOGI(kTag, "Boot count: %lu", static_cast<unsigned long>(boot_count));

  if (!audio_.begin() || !display_.begin() || !metadata_.begin() || !network_.begin() ||
      !voice_.begin()) {
    return false;
  }

  onStreamTrimChanged(storage_.networkSettings().stream_trim_ms);
  onEqSettingsChanged(storage_.networkSettings());
  return true;
}

bool Application::startTasks() {
  return startHeartbeatTask(kAppConfig.heartbeat_period_ms, &heartbeat_task_);
}

}  // namespace snapcast
