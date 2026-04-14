#include "voice_manager.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "../include/minimp3.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <driver/i2s_std.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "app_config.h"
#include "application.h"
#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "voice";
constexpr size_t kWsInlineBufferBytes = 2048;
constexpr size_t kWsMaxPayloadBytes = 16384;

constexpr EventBits_t kWsEvtAuthRequired = BIT0;
constexpr EventBits_t kWsEvtAuthOk = BIT1;
constexpr EventBits_t kWsEvtRunStart = BIT2;
constexpr EventBits_t kWsEvtRunEnd = BIT3;
constexpr EventBits_t kWsEvtError = BIT4;
constexpr EventBits_t kWsEvtAuthInvalid = BIT5;

constexpr int kMicFrameSamples = 320;
constexpr int kMicRawChannels = 2;
constexpr uint32_t kVoiceBootGuardMs = 30000;
constexpr uint32_t kVoiceIdleNoStreamMs = 2200;
constexpr int64_t kMicRawDiagWindowUs = 30000000LL;
constexpr uint32_t kWakeTimeoutS = 12;
constexpr float kAssistVolumeMultiplier = 2.0f;
constexpr int kAssistNoiseSuppLevel = 2;   // 0=off, 4=max (traitement côté HA)
constexpr int kAssistAutoGainDbfs = 31;    // AGC 0=off, 31=max (traitement côté HA)
constexpr const char *kAssistLanguage = "fr-FR";
constexpr bool kAssistUseWakeWordStage = true;
constexpr bool kMicUseMonoAverage = true;
// Temps supplémentaire après le wake word pour STT + intent + TTS
constexpr uint32_t kPostWakeSlackS = 25;
constexpr uint32_t kVoiceSampleRateHz = 16000;
constexpr int kMicRightShiftToS16 = 12;
constexpr int kMicOutputAttenuationShift = 0;
constexpr int32_t kMicDcFilterShift = 9;
constexpr int32_t kMicActiveThreshold = 350;
constexpr bool kMicForceLeftChannel = true;

constexpr i2s_port_t kMicI2SPort = I2S_NUM_1;

static int16_t rawToS16(int32_t raw) {
  // Typical digital MEMS microphones output 24-bit signed samples packed in 32-bit slots.
  int32_t value = raw >> kMicRightShiftToS16;

  value = std::max<int32_t>(-32768, std::min<int32_t>(32767, value));
  return static_cast<int16_t>(value);
}

static bool hasWifiIp() {
  IPAddress ip = WiFi.localIP();
  return WiFi.status() == WL_CONNECTED && ip[0] != 0;
}

static bool isBareWakeWordUtterance(const char *text) {
  if (!text) {
    return false;
  }

  const char *start = text;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
    start++;
  }

  size_t len = strlen(start);
  while (len > 0) {
    const char c = start[len - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.' || c == '!' || c == '?') {
      len--;
      continue;
    }
    break;
  }

  if (len != 9) {
    return false;
  }

  static constexpr char kWakeWord[] = "tournesol";
  for (size_t i = 0; i < len; i++) {
    if (static_cast<char>(tolower(static_cast<unsigned char>(start[i]))) != kWakeWord[i]) {
      return false;
    }
  }
  return true;
}

static bool isLikelyNoCommandUtterance(const char *text) {
  if (!text) {
    return true;
  }

  const char *start = text;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
    start++;
  }

  size_t len = strlen(start);
  while (len > 0) {
    const char c = start[len - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      len--;
      continue;
    }
    break;
  }

  if (len == 0) {
    return true;
  }

  if (start[0] == '[' && start[len - 1] == ']') {
    return true;
  }

  char lower[160] = {0};
  const size_t n = std::min(len, sizeof(lower) - 1);
  for (size_t i = 0; i < n; i++) {
    lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(start[i])));
  }
  lower[n] = '\0';

  if (strcmp(lower, "ok") == 0 || strcmp(lower, "okay") == 0 ||
      strcmp(lower, "oh") == 0) {
    return true;
  }

  if (strstr(lower, "no audio") || strstr(lower, "silence") ||
      strstr(lower, "inhaling") ||
      strstr(lower, "transcribing the attached audio")) {
    return true;
  }

  return false;
}
}

bool VoiceManager::begin() {
  if (running_) {
    return true;
  }

  ws_events_ = xEventGroupCreate();
  if (!ws_events_) {
    LOGE(kTag, "Cannot create websocket event group");
    return false;
  }

  running_ = true;
  boot_time_us_ = esp_timer_get_time();
  no_stream_since_us_ = 0;
  BaseType_t task_ok = xTaskCreatePinnedToCore(taskEntry,
                                                "voice_runtime",
                                                36 * 1024,
                                                this,
                                                3,
                                                &task_,
                                                1);
  if (task_ok != pdPASS) {
    LOGE(kTag, "Cannot create voice runtime task");
    running_ = false;
    vEventGroupDelete(ws_events_);
    ws_events_ = nullptr;
    return false;
  }

  LOGI(kTag, "Voice runtime task started");
  return true;
}

void VoiceManager::tick() {
  // Runtime voice processing lives in a dedicated task.
}

void VoiceManager::taskEntry(void *ctx) {
  VoiceManager *self = static_cast<VoiceManager *>(ctx);
  if (self) {
    self->taskLoop();
  }
  vTaskDelete(nullptr);
}

bool VoiceManager::loadRuntimeConfig(RuntimeVoiceConfig &cfg) const {
  const VoiceSettings &vs = Application::instance().storage().voiceSettings();
  memset(&cfg, 0, sizeof(cfg));

  strncpy(cfg.host, vs.ha_host, sizeof(cfg.host) - 1);
  strncpy(cfg.token, vs.ha_token, sizeof(cfg.token) - 1);
  strncpy(cfg.pipeline_id, vs.ha_pipeline_id, sizeof(cfg.pipeline_id) - 1);
  cfg.port = vs.ha_port;

  if (cfg.host[0] == '\0' || cfg.port <= 0 || cfg.port > 65535 ||
      strlen(cfg.token) < 20 || cfg.pipeline_id[0] == '\0') {
    return false;
  }
  return true;
}

bool VoiceManager::micInitIfNeeded() {
  if (kAppConfig.audio.data_in < 0) {
    LOGW(kTag, "Microphone DIN pin is not configured");
    return false;
  }
  if (mic_inited_) {
    return true;
  }

  const i2s_chan_config_t channel_config = {
      .id = kMicI2SPort,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 8,
      .dma_frame_num = 256,
      .auto_clear = true,
  };

  esp_err_t err = i2s_new_channel(&channel_config, nullptr, &mic_rx_channel_);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_new_channel (rx) failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kVoiceSampleRateHz);
  clk_cfg.clk_src = I2S_CLK_SRC_APLL;

  i2s_std_config_t std_cfg = {
      .clk_cfg = clk_cfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(kAppConfig.audio.bclk),
          .ws = static_cast<gpio_num_t>(kAppConfig.audio.ws),
          .dout = I2S_GPIO_UNUSED,
          .din = static_cast<gpio_num_t>(kAppConfig.audio.data_in),
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };

  err = i2s_channel_init_std_mode(mic_rx_channel_, &std_cfg);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_channel_init_std_mode (rx) failed: %s", esp_err_to_name(err));
    i2s_del_channel(mic_rx_channel_);
    mic_rx_channel_ = nullptr;
    return false;
  }

  err = i2s_channel_enable(mic_rx_channel_);
  if (err != ESP_OK) {
    LOGE(kTag, "i2s_channel_enable (rx) failed: %s", esp_err_to_name(err));
    i2s_del_channel(mic_rx_channel_);
    mic_rx_channel_ = nullptr;
    return false;
  }

  mic_inited_ = true;
  LOGI(kTag,
       "MIC init OK (I2S legacy) BCLK=%d WS=%d DIN=%d @ %lu Hz",
       kAppConfig.audio.bclk,
       kAppConfig.audio.ws,
       kAppConfig.audio.data_in,
      static_cast<unsigned long>(kVoiceSampleRateHz));
  return true;
}

void VoiceManager::micDeinitIfNeeded() {
  if (!mic_inited_) {
    return;
  }
  if (mic_rx_channel_) {
    i2s_channel_disable(mic_rx_channel_);
    i2s_del_channel(mic_rx_channel_);
    mic_rx_channel_ = nullptr;
  }
  mic_inited_ = false;
  LOGI(kTag, "MIC deinit done");
}

bool VoiceManager::openWebsocket(const RuntimeVoiceConfig &cfg) {
  closeWebsocket();

  if (!ws_client_.connect(cfg.host, static_cast<uint16_t>(cfg.port))) {
    LOGW(kTag, "Cannot connect TCP to HA %s:%d", cfg.host, static_cast<int>(cfg.port));
    return false;
  }

  const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
  String req;
  req.reserve(512);
  req += "GET /api/websocket HTTP/1.1\r\n";
  req += "Host: ";
  req += cfg.host;
  req += ":";
  req += String(static_cast<int>(cfg.port));
  req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: ";
  req += ws_key;
  req += "\r\nSec-WebSocket-Version: 13\r\n\r\n";

  ws_client_.print(req);

  String resp;
  resp.reserve(512);
  uint32_t start_ms = millis();
  while ((millis() - start_ms) < 3000) {
    while (ws_client_.available() > 0) {
      char c = static_cast<char>(ws_client_.read());
      resp += c;
      if (resp.endsWith("\r\n\r\n")) {
        break;
      }
    }
    if (resp.endsWith("\r\n\r\n")) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (resp.indexOf(" 101 ") < 0 || resp.indexOf("Upgrade: websocket") < 0) {
    LOGE(kTag, "WS handshake failed: %.120s", resp.c_str());
    closeWebsocket();
    return false;
  }

  ws_connected_ = true;
  ws_client_.setNoDelay(true);
  LOGI(kTag, "HA websocket connected");
  return true;
}

void VoiceManager::closeWebsocket() {
  if (ws_client_.connected()) {
    ws_client_.stop();
  }
  ws_connected_ = false;
  ws_rx_len_ = 0;
}

bool VoiceManager::readBytesWithTimeout(uint8_t *dst, size_t len, uint32_t timeout_ms) {
  size_t got = 0;
  uint32_t start_ms = millis();
  while (got < len && (millis() - start_ms) < timeout_ms) {
    int avail = ws_client_.available();
    if (avail > 0) {
      int c = ws_client_.read();
      if (c >= 0) {
        dst[got++] = static_cast<uint8_t>(c);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  return got == len;
}

bool VoiceManager::sendWsFrame(uint8_t opcode, const uint8_t *payload, size_t len) {
  if (!ws_connected_ || !payload) {
    return false;
  }

  uint8_t header[14] = {0};
  size_t hlen = 0;
  header[hlen++] = static_cast<uint8_t>(0x80 | (opcode & 0x0F));

  if (len < 126) {
    header[hlen++] = static_cast<uint8_t>(0x80 | len);
  } else if (len <= 0xFFFF) {
    header[hlen++] = 0x80 | 126;
    header[hlen++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[hlen++] = static_cast<uint8_t>(len & 0xFF);
  } else {
    return false;
  }

  uint8_t mask[4] = {
      static_cast<uint8_t>(esp_random() & 0xFF),
      static_cast<uint8_t>(esp_random() & 0xFF),
      static_cast<uint8_t>(esp_random() & 0xFF),
      static_cast<uint8_t>(esp_random() & 0xFF),
  };
  memcpy(header + hlen, mask, sizeof(mask));
  hlen += sizeof(mask);

  if (ws_client_.write(header, hlen) != static_cast<int>(hlen)) {
    ws_connected_ = false;
    return false;
  }

  // Envoi du payload masqué par blocs (évite 600+ appels write(byte) qui bloquent)
  constexpr size_t kChunk = 256;
  uint8_t chunk_buf[kChunk];
  for (size_t off = 0; off < len; off += kChunk) {
    size_t n = len - off;
    if (n > kChunk) {
      n = kChunk;
    }
    for (size_t i = 0; i < n; i++) {
      chunk_buf[i] = payload[off + i] ^ mask[(off + i) & 0x3];
    }
    if (ws_client_.write(chunk_buf, n) != static_cast<int>(n)) {
      ws_connected_ = false;
      return false;
    }
  }

  return true;
}

bool VoiceManager::pollWsFrames(uint32_t max_frames) {
  if (!ws_connected_) {
    return false;
  }

  if (ws_frame_buffer_.size() < kWsInlineBufferBytes + 1) {
    ws_frame_buffer_.resize(kWsInlineBufferBytes + 1);
  }

  uint32_t processed = 0;
  while (processed < max_frames && ws_client_.available() >= 2) {
    uint8_t hdr[2] = {0};
    if (!readBytesWithTimeout(hdr, 2, 50)) {
      return false;
    }

    uint8_t opcode = static_cast<uint8_t>(hdr[0] & 0x0F);
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = static_cast<uint64_t>(hdr[1] & 0x7F);

    if (payload_len == 126) {
      uint8_t ext[2] = {0};
      if (!readBytesWithTimeout(ext, 2, 50)) {
        return false;
      }
      payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
      uint8_t ext[8] = {0};
      if (!readBytesWithTimeout(ext, 8, 50)) {
        return false;
      }
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | ext[i];
      }
    }

    uint8_t mask[4] = {0};
    if (masked) {
      if (!readBytesWithTimeout(mask, 4, 50)) {
        return false;
      }
    }

    if (payload_len > kWsMaxPayloadBytes) {
      LOGE(kTag, "HA websocket frame too large: %llu bytes > %u, discarding",
           static_cast<unsigned long long>(payload_len),
           static_cast<unsigned>(kWsMaxPayloadBytes));
      for (uint64_t i = 0; i < payload_len; i++) {
        uint8_t tmp = 0;
        if (!readBytesWithTimeout(&tmp, 1, 50)) {
          return false;
        }
      }
      processed++;
      continue;
    }

    const size_t needed = static_cast<size_t>(payload_len) + 1;
    if (ws_frame_buffer_.size() < needed) {
      ws_frame_buffer_.resize(needed);
    }

    uint8_t *frame_buf = ws_frame_buffer_.data();
    if (!readBytesWithTimeout(frame_buf, static_cast<size_t>(payload_len), 200)) {
      return false;
    }

    if (masked) {
      for (uint64_t i = 0; i < payload_len; i++) {
        frame_buf[i] ^= mask[i & 0x3];
      }
    }

    frame_buf[payload_len] = '\0';

    if (opcode == 0x1) {
      parseWsJson(reinterpret_cast<const char *>(frame_buf), static_cast<int>(payload_len));
    } else if (opcode == 0x8) {
      ws_connected_ = false;
      xEventGroupSetBits(ws_events_, kWsEvtError);
      return false;
    } else if (opcode == 0x9) {
      // ping -> pong
      sendWsFrame(0xA, frame_buf, static_cast<size_t>(payload_len));
    }

    processed++;
  }

  return ws_connected_;
}

void VoiceManager::sendAuth(const char *token) {
  if (!ws_connected_ || !token) {
    return;
  }
  String payload = "{\"type\":\"auth\",\"access_token\":\"";
  payload += token;
  payload += "\"}";
  sendWsFrame(0x1, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
}

void VoiceManager::sendAssistRun(const char *pipeline_id) {
  if (!ws_connected_ || !pipeline_id) {
    return;
  }

  char volume_str[24] = {0};
  snprintf(volume_str, sizeof(volume_str), "%.2f", static_cast<double>(kAssistVolumeMultiplier));

    String payload =
      "{\"id\":1001,\"type\":\"assist_pipeline/run\",\"start_stage\":\"";
    payload += kAssistUseWakeWordStage ? "wake_word" : "stt";
    payload += "\",";
  payload += "\"end_stage\":\"tts\",\"pipeline\":\"";
  payload += pipeline_id;
  payload += "\",\"input\":{";  
  payload += "\"sample_rate\":";
  payload += String(kVoiceSampleRateHz);
  payload += ",\"timeout\":";
  payload += String(kWakeTimeoutS);
  payload += ",\"noise_suppression_level\":";
  payload += String(kAssistNoiseSuppLevel);
  payload += ",\"auto_gain_dbfs\":";
  payload += String(kAssistAutoGainDbfs);
  payload += ",\"volume_multiplier\":";
  payload += volume_str;
  payload += "}}";

      LOGI(kTag, "HA assist run params: stage=%s timeout=%lus noise=%d agc=%d vol=%s",
        kAssistUseWakeWordStage ? "wake_word" : "stt",
        static_cast<unsigned long>(kWakeTimeoutS),
        kAssistNoiseSuppLevel, kAssistAutoGainDbfs, volume_str);

  sendWsFrame(0x1, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
}

void VoiceManager::parseWsJson(const char *data, int len) {
  if (!data || len <= 0) {
    return;
  }

  const size_t doc_capacity = static_cast<size_t>(len) + 2048;
  DynamicJsonDocument doc(doc_capacity < 4096 ? 4096 : doc_capacity);
  auto err = deserializeJson(doc, data, static_cast<size_t>(len));
  if (err) {
    LOGW(kTag, "Failed to parse HA JSON frame (%d bytes): %s", len, err.c_str());
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "auth_required") == 0) {
    xEventGroupSetBits(ws_events_, kWsEvtAuthRequired);
    return;
  }
  if (strcmp(type, "auth_ok") == 0) {
    xEventGroupSetBits(ws_events_, kWsEvtAuthOk);
    return;
  }
  if (strcmp(type, "auth_invalid") == 0) {
    const char *msg = doc["message"] | "(no message)";
    LOGE(kTag, "HA auth_invalid: %s", msg);
    xEventGroupSetBits(ws_events_, kWsEvtAuthInvalid);
    return;
  }

  if (strcmp(type, "result") == 0) {
    bool success = doc["success"] | true;
    if (!success) {
      const char *code = doc["error"]["code"] | "?";
      const char *msg = doc["error"]["message"] | "?";
      LOGE(kTag, "HA command failed: %s - %s", code, msg);
      xEventGroupSetBits(ws_events_, kWsEvtError);
    }
    return;
  }

  if (strcmp(type, "event") == 0) {
    const char *event_type = doc["event"]["type"] | "";
    if (event_type[0] != '\0') {
      LOGI(kTag, "HA assist event: %s", event_type);
    }
    if (strcmp(event_type, "run-start") == 0) {
      binary_handler_id_ = static_cast<uint8_t>(doc["event"]["data"]["runner_data"]["stt_binary_handler_id"] | 0);
      run_start_received_ = true;
      last_stt_text_[0] = '\0';
      const char *pipeline_lang = doc["event"]["data"]["language"] | "?";
      LOGI(kTag, "HA run-start: binary_handler_id=%d lang=%s", static_cast<int>(binary_handler_id_), pipeline_lang);
      xEventGroupSetBits(ws_events_, kWsEvtRunStart);
    } else if (strcmp(event_type, "wake_word-end") == 0) {
      String payload;
      serializeJson(doc["event"]["data"], payload);
      if (payload.length() > 0) {
        LOGI(kTag, "HA wake_word-end data: %s", payload.c_str());
      }
    } else if (strcmp(event_type, "stt-end") == 0) {
      const char *stt_text = doc["event"]["data"]["stt_output"]["text"] |
                             doc["event"]["data"]["text"] | "";
      strncpy(last_stt_text_, stt_text, sizeof(last_stt_text_) - 1);
      last_stt_text_[sizeof(last_stt_text_) - 1] = '\0';
      if (stt_text[0] != '\0') {
        LOGI(kTag, "HA stt-end text: %s", stt_text);
      } else {
        String payload;
        serializeJson(doc["event"]["data"], payload);
        if (payload.length() > 0) {
          LOGI(kTag, "HA stt-end data: %s", payload.c_str());
        }
      }
    } else if (strcmp(event_type, "intent-start") == 0) {
      const char *intent_lang = doc["event"]["data"]["language"] | "?";
      const char *intent_engine = doc["event"]["data"]["engine"] | "?";
      const char *intent_input = doc["event"]["data"]["intent_input"] | "";
      LOGI(kTag, "HA intent-start: engine=%s lang=%s input=%s", intent_engine, intent_lang, intent_input);
    } else if (strcmp(event_type, "intent-end") == 0) {
      const char *intent_name =
          doc["event"]["data"]["intent_output"]["intent"]["intent_type"] |
          doc["event"]["data"]["intent"]["intent_type"] | "";
      const char *speech =
          doc["event"]["data"]["intent_output"]["response"]["speech"]["plain"]["speech"] |
          doc["event"]["data"]["response"]["speech"]["plain"]["speech"] | "";
      if (intent_name[0] != '\0') {
        LOGI(kTag, "HA intent-end intent: %s", intent_name);
      }
      if (speech[0] != '\0') {
        LOGI(kTag, "HA intent-end speech: %s", speech);
      }
    } else if (strcmp(event_type, "tts-end") == 0) {
      const char *url = doc["event"]["data"]["tts_output"]["url"] | "";
      const char *mime = doc["event"]["data"]["tts_output"]["mime_type"] | "";
      if (url[0] != '\0') {
        strncpy(tts_url_, url, sizeof(tts_url_) - 1);
        tts_url_[sizeof(tts_url_) - 1] = '\0';
        strncpy(tts_mime_type_, mime, sizeof(tts_mime_type_) - 1);
        tts_mime_type_[sizeof(tts_mime_type_) - 1] = '\0';
        LOGI(kTag, "TTS url: %s mime: %s", tts_url_, tts_mime_type_);
      }
    } else if (strcmp(event_type, "run-end") == 0) {
      const char *stt_text = doc["event"]["data"]["stt_output"]["text"] | "";
      const char *effective_stt_text = stt_text[0] != '\0' ? stt_text : last_stt_text_;
      const char *intent_speech =
          doc["event"]["data"]["intent_output"]["response"]["speech"]["plain"]["speech"] | "";
      const char *intent_name = doc["event"]["data"]["intent_output"]["intent"]["intent_type"] | "";
      const bool bare_wake_word = !kAssistUseWakeWordStage && isBareWakeWordUtterance(effective_stt_text);
        const bool likely_no_command =
          kAssistUseWakeWordStage && effective_stt_text[0] != '\0' && isLikelyNoCommandUtterance(effective_stt_text);
      if (effective_stt_text[0] != '\0') {
        LOGI(kTag, "HA stt text: %s", effective_stt_text);
      }
      if (intent_name[0] != '\0') {
        LOGI(kTag, "HA intent: %s", intent_name);
      }
      if (intent_speech[0] != '\0') {
        LOGI(kTag, "HA response: %s", intent_speech);
      }
      if (bare_wake_word) {
        // In STT-direct mode, users may still say only the wake word by habit.
        // Ignore this specific utterance to avoid pointless fallback TTS responses.
        tts_url_[0] = '\0';
        tts_mime_type_[0] = '\0';
        LOGI(kTag, "Ignoring bare wake word utterance in STT mode");
      }
      if (likely_no_command) {
        // Keep TTS playback enabled in wake-word mode even for non-command
        // transcripts so the user still hears HA feedback (e.g. "repeat").
        LOGI(kTag, "Likely non-command STT transcript after wake word: %s", effective_stt_text);
      }
      xEventGroupSetBits(ws_events_, kWsEvtRunEnd);
    } else if (strcmp(event_type, "error") == 0) {
      const char *code = doc["event"]["data"]["code"] | "unknown";
      const char *msg = doc["event"]["data"]["message"] | "n/a";
      LOGW(kTag, "Assist error: %s - %s", code, msg);
      xEventGroupSetBits(ws_events_, kWsEvtError);
    }
  }
}

void VoiceManager::sendBinaryFrame(const uint8_t *payload, size_t len) {
  if (!ws_connected_ || !payload || len == 0 || !run_start_received_) {
    return;
  }

  uint8_t *packet = static_cast<uint8_t *>(malloc(len + 1));
  if (!packet) {
    return;
  }
  packet[0] = binary_handler_id_;
  memcpy(packet + 1, payload, len);
  sendWsFrame(0x2, packet, len + 1);
  free(packet);
}

void VoiceManager::sendBinaryEndMarker() {
  if (!ws_connected_ || !run_start_received_) {
    return;
  }
  uint8_t marker = binary_handler_id_;
  sendWsFrame(0x2, &marker, 1);
}

bool VoiceManager::waitForWsBits(EventBits_t bits, uint32_t timeout_ms, bool clear_on_exit) {
  uint32_t start_ms = millis();
  while ((millis() - start_ms) < timeout_ms) {
    if (ws_connected_) {
      pollWsFrames();
    }
    EventBits_t current = xEventGroupGetBits(ws_events_);
    if ((current & bits) != 0) {
      if (clear_on_exit) {
        xEventGroupClearBits(ws_events_, bits);
      }
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

void VoiceManager::streamMicUntilEnd(int64_t deadline_us) {
  int32_t raw_buf[kMicFrameSamples * kMicRawChannels] = {0};
  int16_t mic_buf[kMicFrameSamples] = {0};
  size_t bytes_read = 0;

  uint32_t ok_frames = 0;
  uint32_t zero_frames = 0;
  uint32_t err_frames = 0;
  uint32_t active_frames_1s = 0;

  int32_t peak_l_30s = 0;
  int32_t peak_r_30s = 0;
  uint32_t active_frames_30s = 0;
  uint32_t total_frames_30s = 0;
  uint32_t selected_left_30s = 0;
  uint32_t selected_right_30s = 0;
  uint64_t sum_sq_out_30s = 0;
  uint32_t sample_count_30s = 0;

  int32_t dc_l = mic_dc_l_;
  int32_t dc_r = mic_dc_r_;

  esp_err_t last_err = ESP_OK;
  int64_t diag_deadline_us = esp_timer_get_time() + kMicRawDiagWindowUs;
  bool diag_done = false;

  while (running_) {
    if (ws_connected_) {
      pollWsFrames();
    }
    EventBits_t bits = xEventGroupGetBits(ws_events_);
    if ((bits & kWsEvtRunEnd) || (bits & kWsEvtError)) {
      break;
    }

    if (Application::instance().isStreamPlaying()) {
      LOGI(kTag, "MA indicates stream playing, stopping voice capture");
      break;
    }

    if (!ws_connected_) {
      LOGW(kTag, "WS disconnected while streaming mic");
      break;
    }

    if (esp_timer_get_time() > deadline_us) {
      LOGW(kTag, "Voice session timeout reached");
      break;
    }

    esp_err_t err = i2s_channel_read(mic_rx_channel_, raw_buf, sizeof(raw_buf), &bytes_read, pdMS_TO_TICKS(120));
    if (err == ESP_OK && bytes_read > 0) {
      int raw_samples = static_cast<int>(bytes_read / sizeof(int32_t));
      int frames = raw_samples / kMicRawChannels;
      if (frames > kMicFrameSamples) {
        frames = kMicFrameSamples;
      }

      for (int i = 0; i < frames; i++) {
        int16_t l_raw = rawToS16(raw_buf[i * kMicRawChannels]);
        int16_t r_raw = rawToS16(raw_buf[(i * kMicRawChannels) + 1]);

        dc_l += (static_cast<int32_t>(l_raw) - dc_l) >> kMicDcFilterShift;
        dc_r += (static_cast<int32_t>(r_raw) - dc_r) >> kMicDcFilterShift;

        int16_t l = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, static_cast<int32_t>(l_raw) - dc_l)));
        int16_t r = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, static_cast<int32_t>(r_raw) - dc_r)));

        const int32_t abs_l = l < 0 ? -l : l;
        const int32_t abs_r = r < 0 ? -r : r;

        if (abs_l > peak_l_30s) {
          peak_l_30s = abs_l;
        }
        if (abs_r > peak_r_30s) {
          peak_r_30s = abs_r;
        }

        // Build a stable mono stream for wake-word/STT by averaging L/R.
        // This avoids per-sample channel switching artifacts and remains robust
        // when one side is quieter.
        bool use_right = false;
        int16_t chosen = 0;
        if (kMicUseMonoAverage) {
          chosen = static_cast<int16_t>((static_cast<int32_t>(l) + static_cast<int32_t>(r)) / 2);
        } else {
          if (!kMicForceLeftChannel) {
            const bool right_suspicious = (abs_r > 28000) && (abs_l < 22000);
            use_right = !right_suspicious && (abs_r > (abs_l + 64));
          }
          chosen = use_right ? r : l;
        }

        // Soft-limiter only on the selected signal sent to HA (keep diagnostic L/R raw).
        constexpr int32_t kSoftLimitThreshold = 24000;
        int32_t abs_chosen = chosen < 0 ? -chosen : chosen;
        if (abs_chosen > kSoftLimitThreshold) {
          chosen = (chosen > 0) ? static_cast<int16_t>(kSoftLimitThreshold)
                                : static_cast<int16_t>(-kSoftLimitThreshold);
        }
        mic_buf[i] = static_cast<int16_t>(chosen >> kMicOutputAttenuationShift);

        if (use_right) {
          selected_right_30s++;
        } else {
          selected_left_30s++;
        }

        int32_t abs_out = mic_buf[i] < 0 ? -mic_buf[i] : mic_buf[i];
        sum_sq_out_30s += static_cast<uint64_t>(abs_out) * static_cast<uint64_t>(abs_out);
        sample_count_30s++;

        if (abs_out > kMicActiveThreshold) {
          active_frames_1s++;
          active_frames_30s++;
        }
        total_frames_30s++;
      }

      // Blanking post-TTS : envoyer silence au lieu du mic pendant le retour acoustique
      if (tts_blank_until_us_ > 0 && esp_timer_get_time() < tts_blank_until_us_) {
        memset(mic_buf, 0, static_cast<size_t>(frames) * sizeof(int16_t));
      }
      sendBinaryFrame(reinterpret_cast<const uint8_t *>(mic_buf),
                      static_cast<size_t>(frames * static_cast<int>(sizeof(int16_t))));
      ok_frames++;
    } else if (err == ESP_OK) {
      zero_frames++;
    } else {
      err_frames++;
      last_err = err;
    }

    int64_t now_us = esp_timer_get_time();
    if (!diag_done && now_us >= diag_deadline_us) {
      uint32_t rms_out_30s = 0;
      if (sample_count_30s > 0) {
        rms_out_30s = static_cast<uint32_t>(sqrt(static_cast<double>(sum_sq_out_30s) / static_cast<double>(sample_count_30s)));
      }

      LOGI(kTag,
           "MIC raw diag 30s: peakL=%ld peakR=%ld rms=%lu active=%lu/%lu selL=%lu selR=%lu",
           static_cast<long>(peak_l_30s),
           static_cast<long>(peak_r_30s),
           static_cast<unsigned long>(rms_out_30s),
           static_cast<unsigned long>(active_frames_30s),
           static_cast<unsigned long>(total_frames_30s),
           static_cast<unsigned long>(selected_left_30s),
           static_cast<unsigned long>(selected_right_30s));
      diag_done = true;
    }
  }

  sendBinaryEndMarker();
  mic_dc_l_ = dc_l;
  mic_dc_r_ = dc_r;
}

void VoiceManager::playTtsAudio(const RuntimeVoiceConfig &cfg) {
  if (tts_url_[0] == '\0') {
    return;
  }

  LOGI(kTag, "TTS: fetching %s", tts_url_);

  WiFiClient http;
  if (!http.connect(cfg.host, static_cast<uint16_t>(cfg.port))) {
    LOGW(kTag, "TTS HTTP connect failed");
    return;
  }

  // Requête HTTP GET
  String req = "GET ";
  req += tts_url_;
  req += " HTTP/1.1\r\nHost: ";
  req += cfg.host;
  req += ":";
  req += String(static_cast<int>(cfg.port));
  req += "\r\nAuthorization: Bearer ";
  req += cfg.token;
  req += "\r\nConnection: close\r\n\r\n";
  http.print(req);

  // Lecture entêtes HTTP jusqu'à \r\n\r\n
  bool header_done = false;
  bool status_ok = false;
  String header_buf;
  header_buf.reserve(512);
  uint32_t hstart = millis();
  while (!header_done && (millis() - hstart) < 5000) {
    while (http.available()) {
      char c = static_cast<char>(http.read());
      header_buf += c;
      if (header_buf.endsWith("\r\n\r\n")) {
        header_done = true;
        break;
      }
      // éviter accumulation infinie en cas de header très long
      if (header_buf.length() > 2048) {
        header_done = true;
        break;
      }
    }
    if (!header_done) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (!header_done) {
    LOGW(kTag, "TTS: headers not received");
    http.stop();
    return;
  }

  // Vérifier statut 200
  if (header_buf.indexOf(" 200 ") < 0) {
    LOGW(kTag, "TTS: HTTP error: %.60s", header_buf.c_str());
    http.stop();
    return;
  }
  status_ok = true;
  (void)status_ok;

  // Déterminer le format audio depuis le mime_type capturé
  bool is_mp3 = (strstr(tts_mime_type_, "mpeg") != nullptr ||
                 strstr(tts_mime_type_, "mp3") != nullptr);
  bool is_wav = (!is_mp3 && strstr(tts_mime_type_, "wav") != nullptr);

  // Aussi vérifier Content-Type dans les entêtes (priorité sur mime_type capturé)
  if (header_buf.indexOf("audio/mpeg") >= 0 || header_buf.indexOf("audio/mp3") >= 0) {
    is_mp3 = true;
    is_wav = false;
  } else if (header_buf.indexOf("audio/wav") >= 0 || header_buf.indexOf("audio/x-wav") >= 0) {
    is_wav = true;
    is_mp3 = false;
  }

  LOGI(kTag, "TTS: format %s", is_mp3 ? "MP3" : is_wav ? "WAV" : "unknown");

  if (is_mp3) {
    // --- Décodage MP3 avec minimp3 ---
    // Buffers sur PSRAM (si disponible)
    constexpr size_t kReadBufSize = 4096;
    uint8_t *mp3_buf = static_cast<uint8_t *>(heap_caps_malloc(kReadBufSize * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mp3_buf) {
      mp3_buf = static_cast<uint8_t *>(malloc(kReadBufSize * 2));
    }
    if (!mp3_buf) {
      LOGE(kTag, "TTS: out of memory for MP3 buf");
      http.stop();
      return;
    }

    int16_t *pcm_out = static_cast<int16_t *>(heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_out) {
      pcm_out = static_cast<int16_t *>(malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    }
    if (!pcm_out) {
      LOGE(kTag, "TTS: out of memory for PCM buf");
      free(mp3_buf);
      http.stop();
      return;
    }

    mp3dec_t *dec = static_cast<mp3dec_t *>(heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dec) {
      dec = static_cast<mp3dec_t *>(malloc(sizeof(mp3dec_t)));
    }
    if (!dec) {
      LOGE(kTag, "TTS: out of memory for MP3 decoder");
      free(pcm_out);
      free(mp3_buf);
      http.stop();
      return;
    }

    mp3dec_init(dec);

    int mp3_len = 0;
    bool format_set = false;
    uint32_t total_samples = 0;
    uint32_t out_hz = 0;

    uint32_t tstart = millis();
    while ((millis() - tstart) < 10000) {
      // Lire des données HTTP dans le buffer
      int avail = http.available();
      if (avail > 0) {
        size_t space = kReadBufSize * 2 - static_cast<size_t>(mp3_len);
        if (space > 0) {
          size_t to_read = static_cast<size_t>(avail) < space ? static_cast<size_t>(avail) : space;
          int n = http.read(mp3_buf + mp3_len, static_cast<int>(to_read));
          if (n > 0) {
            mp3_len += n;
            tstart = millis();  // reset timeout à chaque donnée reçue
          }
        }
      } else if (!http.connected()) {
        // Plus de données, connexion fermée
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }

      // Décoder les frames disponibles
      while (mp3_len > 0) {
        mp3dec_frame_info_t info = {0, 0, 0, 0, 0, 0};
        int samples = mp3dec_decode_frame(dec, mp3_buf, mp3_len, pcm_out, &info);

        if (info.frame_bytes == 0) {
          // Plus assez de données pour décoder une frame
          break;
        }

        if (samples > 0) {
          if (!format_set) {
            // Toujours émettre en mono pour rester sous pcm_chunk_max_size (4096)
            out_hz = static_cast<uint32_t>(info.hz);
            Application::instance().startVoicePlayback(out_hz, 1);
            format_set = true;
            LOGI(kTag, "TTS MP3: %dHz ch=%d bitrate=%dkbps", info.hz, info.channels, info.bitrate_kbps);
          }

          // Convertir en mono si stéréo
          if (info.channels == 2) {
            for (int i = 0; i < samples; i++) {
              pcm_out[i] = static_cast<int16_t>(
                  (static_cast<int32_t>(pcm_out[i * 2]) + static_cast<int32_t>(pcm_out[i * 2 + 1])) / 2);
            }
          }

          // Aligner sur 4 octets (samples * 2 doit être multiple de 4 → samples pair)
          int aligned_samples = samples & ~1;
          if (aligned_samples > 0) {
            Application::instance().feedVoicePcm(
                reinterpret_cast<const uint8_t *>(pcm_out),
                static_cast<size_t>(aligned_samples) * sizeof(int16_t));
          }
          total_samples += static_cast<uint32_t>(aligned_samples);
        }

        // Décaler le buffer
        int consumed = info.frame_bytes;
        memmove(mp3_buf, mp3_buf + consumed, static_cast<size_t>(mp3_len - consumed));
        mp3_len -= consumed;
      }
    }

    // Attendre la fin de lecture reelle cote sortie audio.
    if (format_set && out_hz > 0 && total_samples > 0) {
      uint32_t duration_ms = (total_samples * 1000u) / out_hz;
      LOGI(kTag, "TTS MP3: %lu samples, ~%lums queued, waiting for drain...",
           static_cast<unsigned long>(total_samples),
           static_cast<unsigned long>(duration_ms));
      Application::instance().waitForVoicePlaybackDrain(duration_ms + 1500, 120);
    }

    free(dec);
    free(pcm_out);
    free(mp3_buf);

  } else if (is_wav) {
    // --- Lecture WAV brut (PCM 16-bit) ---
    uint8_t hdr[44] = {0};
    size_t got = 0;
    uint32_t wstart = millis();
    while (got < 44 && (millis() - wstart) < 3000) {
      if (http.available()) {
        int c = http.read();
        if (c >= 0) {
          hdr[got++] = static_cast<uint8_t>(c);
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    if (got < 44 || hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') {
      LOGW(kTag, "TTS: invalid WAV header");
      http.stop();
      return;
    }
    uint16_t wav_ch = static_cast<uint16_t>(hdr[22] | (hdr[23] << 8));
    uint32_t wav_hz = static_cast<uint32_t>(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    uint16_t wav_bits = static_cast<uint16_t>(hdr[34] | (hdr[35] << 8));
    LOGI(kTag, "TTS WAV: %luHz ch=%u bits=%u", static_cast<unsigned long>(wav_hz), wav_ch, wav_bits);

    if (wav_bits != 16 || (wav_ch != 1 && wav_ch != 2)) {
      LOGW(kTag, "TTS WAV: unsupported format");
      http.stop();
      return;
    }

    Application::instance().startVoicePlayback(wav_hz, wav_ch);

    constexpr size_t kChunk = 4096;
    uint8_t *chunk = static_cast<uint8_t *>(malloc(kChunk));
    if (!chunk) {
      LOGE(kTag, "TTS WAV: out of memory");
      http.stop();
      Application::instance().stopVoicePlayback();
      return;
    }

    uint32_t total_pcm_bytes = 0;
    uint32_t wts = millis();
    while ((millis() - wts) < 10000) {
      int n = http.available();
      if (n > 0) {
        size_t to_read = static_cast<size_t>(n) < kChunk ? static_cast<size_t>(n) : kChunk;
        int read_n = http.read(chunk, static_cast<int>(to_read));
        if (read_n > 0) {
          Application::instance().feedVoicePcm(chunk, static_cast<size_t>(read_n));
          total_pcm_bytes += static_cast<uint32_t>(read_n);
          wts = millis();
        }
      } else if (!http.connected()) {
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }

    free(chunk);

    // Attendre fin lecture reelle cote sortie audio.
    uint32_t frame_bytes = static_cast<uint32_t>(wav_ch) * 2u;
    if (frame_bytes > 0 && wav_hz > 0 && total_pcm_bytes > 0) {
      uint32_t dur_ms = (total_pcm_bytes / frame_bytes * 1000u) / wav_hz;
      LOGI(kTag, "TTS WAV: %lu bytes, ~%lums queued, waiting for drain...", static_cast<unsigned long>(total_pcm_bytes),
           static_cast<unsigned long>(dur_ms));
      Application::instance().waitForVoicePlaybackDrain(dur_ms + 1500, 120);
    }

  } else {
    LOGW(kTag, "TTS: unknown audio format, skipping playback");
  }

  http.stop();
  // Armer le blanking mic pour couvrir le retour acoustique résiduel
  tts_blank_until_us_ = esp_timer_get_time() + 800000LL;  // 800 ms
  LOGI(kTag, "playTtsAudio done, stopping playback");
  Application::instance().stopVoicePlayback();
}

void VoiceManager::runAssistSession(const RuntimeVoiceConfig &cfg) {
  xEventGroupClearBits(ws_events_, 0xFFFF);
  binary_handler_id_ = 0;
  run_start_received_ = false;
  Application::instance().enterVoiceCaptureMode();

  if (!micInitIfNeeded()) {
    Application::instance().leaveVoiceCaptureMode();
    return;
  }

  bool auth_required_ok = false;
  for (int attempt = 1; attempt <= 2 && !auth_required_ok; attempt++) {
    if (attempt > 1) {
      LOGI(kTag, "Retry websocket connection (attempt %d)", attempt);
      closeWebsocket();
      vTaskDelay(pdMS_TO_TICKS(400));
      xEventGroupClearBits(ws_events_, 0xFFFF);
    }
    if (!openWebsocket(cfg)) {
      continue;
    }

    auth_required_ok = waitForWsBits(kWsEvtAuthRequired | kWsEvtError, 5000, false);
    EventBits_t bits = xEventGroupGetBits(ws_events_);
    if ((bits & kWsEvtError) != 0) {
      auth_required_ok = false;
      xEventGroupClearBits(ws_events_, kWsEvtError);
    }
    xEventGroupClearBits(ws_events_, kWsEvtAuthRequired);
  }

  if (!auth_required_ok) {
    LOGW(kTag, "HA auth_required not received");
    closeWebsocket();
    micDeinitIfNeeded();
    Application::instance().leaveVoiceCaptureMode();
    return;
  }

  sendAuth(cfg.token);
  waitForWsBits(kWsEvtAuthOk | kWsEvtAuthInvalid | kWsEvtError, 6000, false);
  EventBits_t bits = xEventGroupGetBits(ws_events_);
  if ((bits & kWsEvtAuthOk) == 0) {
    if (bits & kWsEvtAuthInvalid) {
      LOGE(kTag, "HA token invalide (voir onglet Voix HA)");
    } else {
      LOGW(kTag, "HA auth failed or timed out");
    }
    closeWebsocket();
    micDeinitIfNeeded();
    Application::instance().leaveVoiceCaptureMode();
    return;
  }
  xEventGroupClearBits(ws_events_, kWsEvtAuthOk | kWsEvtAuthInvalid | kWsEvtError);

  sendAssistRun(cfg.pipeline_id);
  waitForWsBits(kWsEvtRunStart | kWsEvtError, 8000, false);
  bits = xEventGroupGetBits(ws_events_);
  if ((bits & kWsEvtRunStart) == 0) {
    LOGW(kTag, "Assist run-start not received");
    closeWebsocket();
    micDeinitIfNeeded();
    Application::instance().leaveVoiceCaptureMode();
    return;
  }
  xEventGroupClearBits(ws_events_, kWsEvtRunStart | kWsEvtError);

  LOGI(kTag, "Assist listening started (pipeline=%s)", cfg.pipeline_id);
  // La deadline couvre : attente du wake word (kWakeTimeoutS) + STT + intent + TTS
  int64_t deadline_us = esp_timer_get_time() +
      static_cast<int64_t>(kWakeTimeoutS + kPostWakeSlackS) * 1000000LL;
  tts_url_[0] = '\0';
  tts_mime_type_[0] = '\0';
  streamMicUntilEnd(deadline_us);
  waitForWsBits(kWsEvtRunEnd, 3000, true);

  closeWebsocket();
  micDeinitIfNeeded();

  // Lire et jouer la réponse TTS si disponible
  if (tts_url_[0] != '\0') {
    playTtsAudio(cfg);
    tts_url_[0] = '\0';
    tts_mime_type_[0] = '\0';
  }
  Application::instance().leaveVoiceCaptureMode();
}

void VoiceManager::taskLoop() {
  uint32_t missing_cfg_log_ms = 0;
  LOGI(kTag, "Voice runtime loop active");

  while (running_) {
    if (!hasWifiIp()) {
      no_stream_since_us_ = 0;
      vTaskDelay(pdMS_TO_TICKS(1500));
      continue;
    }

    RuntimeVoiceConfig cfg = {};
    if (!loadRuntimeConfig(cfg)) {
      uint32_t now_ms = millis();
      if (now_ms - missing_cfg_log_ms > 10000) {
        LOGW(kTag, "Voice runtime idle: HA config incomplete (host/port/token/pipeline)");
        missing_cfg_log_ms = now_ms;
      }
      no_stream_since_us_ = 0;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (Application::instance().isStreamPlaying()) {
      no_stream_since_us_ = 0;
      micDeinitIfNeeded();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if ((esp_timer_get_time() - boot_time_us_) < static_cast<int64_t>(kVoiceBootGuardMs) * 1000LL) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (no_stream_since_us_ == 0) {
      no_stream_since_us_ = esp_timer_get_time();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if ((esp_timer_get_time() - no_stream_since_us_) <
        static_cast<int64_t>(kVoiceIdleNoStreamMs) * 1000LL) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    runAssistSession(cfg);
    no_stream_since_us_ = 0;
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  closeWebsocket();
  micDeinitIfNeeded();
  LOGI(kTag, "Voice runtime loop stopped");
}

}  // namespace snapcast
