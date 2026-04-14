#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <driver/i2s_std.h>
#include <WiFiClient.h>

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace snapcast {

class VoiceManager {
 public:
  bool begin();
  void tick();

 private:
  struct RuntimeVoiceConfig {
    char host[128];
    int32_t port;
    char token[512];
    char pipeline_id[128];
  };

  static void taskEntry(void *ctx);
  void taskLoop();
  bool loadRuntimeConfig(RuntimeVoiceConfig &cfg) const;
  bool micInitIfNeeded();
  void micDeinitIfNeeded();
  bool openWebsocket(const RuntimeVoiceConfig &cfg);
  void closeWebsocket();
  void runAssistSession(const RuntimeVoiceConfig &cfg);
  void streamMicUntilEnd(int64_t deadline_us);
  void playTtsAudio(const RuntimeVoiceConfig &cfg);
  void parseWsJson(const char *data, int len);
  void sendAuth(const char *token);
  void sendAssistRun(const char *pipeline_id);
  void sendBinaryFrame(const uint8_t *payload, size_t len);
  void sendBinaryEndMarker();
  bool sendWsFrame(uint8_t opcode, const uint8_t *payload, size_t len);
  bool pollWsFrames(uint32_t max_frames = 4);
  bool readBytesWithTimeout(uint8_t *dst, size_t len, uint32_t timeout_ms);
  bool waitForWsBits(EventBits_t bits, uint32_t timeout_ms, bool clear_on_exit);

  bool running_ = false;
  bool mic_inited_ = false;
  bool ws_connected_ = false;
  bool run_start_received_ = false;
  uint8_t binary_handler_id_ = 0;
  int64_t boot_time_us_ = 0;
  int64_t no_stream_since_us_ = 0;
  int32_t mic_dc_l_ = 0;   // filtre DC persistant entre sessions
  int32_t mic_dc_r_ = 0;
  int64_t tts_blank_until_us_ = 0; // blanking mic après lecture TTS
  char tts_url_[256] = {0};
  char tts_mime_type_[64] = {0};
  char last_stt_text_[192] = {0};
  int ws_rx_len_ = 0;
  std::vector<uint8_t> ws_frame_buffer_{};

  TaskHandle_t task_ = nullptr;
  EventGroupHandle_t ws_events_ = nullptr;
  i2s_chan_handle_t mic_rx_channel_ = nullptr;
  WiFiClient ws_client_;
};

}  // namespace snapcast
