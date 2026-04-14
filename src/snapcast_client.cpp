#include "snapcast_client.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>

#include "app_config.h"
#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "snapcast";
constexpr uint16_t kSnapcastMessageCodecHeader = 1;
constexpr uint16_t kSnapcastMessageWireChunk = 2;
constexpr uint16_t kSnapcastMessageServerSettings = 3;
constexpr uint16_t kSnapcastMessageTime = 4;
constexpr uint16_t kSnapcastMessageHello = 5;
constexpr uint16_t kSnapcastMessageClientInfo = 7;
constexpr uint16_t kSnapcastRefersToNone = 0;

inline void writeLe16(uint8_t *dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFF);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

inline void writeLe32(uint8_t *dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFF);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

inline int64_t nowUs() { return esp_timer_get_time(); }

inline uint16_t readLe16(const uint8_t *src) {
  return static_cast<uint16_t>(src[0]) |
         (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t readLe32(const uint8_t *src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

struct ServerSettingsCandidate {
  bool valid = false;
  int score = -1;
  uint32_t volume = 0;
  bool muted = false;
  int32_t latency = 0;
  int32_t buffer_ms = 0;
};

bool equalsIgnoreCase(const char *a, const char *b) {
  if (!a || !b) {
    return false;
  }
  return strcasecmp(a, b) == 0;
}

int extractIdentityScore(JsonObjectConst obj, const char *preferred_client_id,
                         const char *preferred_client_name) {
  int score = 0;

  const char *id = obj["id"] | obj["ID"] | obj["clientId"] | obj["identifier"] | nullptr;
  if (preferred_client_id && preferred_client_id[0] != '\0' && id &&
      equalsIgnoreCase(id, preferred_client_id)) {
    score += 100;
  }

  const char *name = obj["name"] | obj["clientName"] | obj["ClientName"] | nullptr;
  if (preferred_client_name && preferred_client_name[0] != '\0' && name &&
      equalsIgnoreCase(name, preferred_client_name)) {
    score += 60;
  }

  return score;
}

bool parseVolumePercent(JsonVariantConst node, uint32_t *out_volume) {
  if (!out_volume || node.isNull()) {
    return false;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();
    if (obj.containsKey("percent")) {
      return parseVolumePercent(obj["percent"], out_volume);
    }
    if (obj.containsKey("volume")) {
      return parseVolumePercent(obj["volume"], out_volume);
    }
    if (obj.containsKey("value")) {
      return parseVolumePercent(obj["value"], out_volume);
    }
    return false;
  }

  if (node.is<float>() || node.is<double>() || node.is<int>() || node.is<long>() ||
      node.is<unsigned long>()) {
    float parsed = node.as<float>();
    if (parsed < 0.0f) {
      parsed = 0.0f;
    }
    if (parsed > 100.0f) {
      parsed = 100.0f;
    }
    *out_volume = static_cast<uint32_t>(lroundf(parsed));
    return true;
  }

  return false;
}

bool parseMutedFlag(JsonVariantConst node, bool *out_muted) {
  if (!out_muted || node.isNull()) {
    return false;
  }

  if (node.is<bool>()) {
    *out_muted = node.as<bool>();
    return true;
  }
  if (node.is<int>() || node.is<long>() || node.is<unsigned long>()) {
    *out_muted = (node.as<long>() != 0);
    return true;
  }
  if (node.is<const char *>()) {
    const char *text = node.as<const char *>();
    if (!text) {
      return false;
    }
    *out_muted = (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0);
    return true;
  }

  return false;
}

bool parseIntField(JsonVariantConst node, int32_t *out_value) {
  if (!out_value || node.isNull()) {
    return false;
  }
  if (node.is<float>() || node.is<double>() || node.is<int>() || node.is<long>() ||
      node.is<unsigned long>()) {
    *out_value = node.as<int32_t>();
    return true;
  }
  return false;
}

void evaluateServerSettingsObject(JsonObjectConst obj, int identity_score,
                                  ServerSettingsCandidate *best) {
  if (!best) {
    return;
  }

  JsonVariantConst volume_node = obj["volume"];

  uint32_t parsed_volume = 0;
  bool has_volume = parseVolumePercent(volume_node, &parsed_volume);
  if (!has_volume) {
    has_volume = parseVolumePercent(obj["percent"], &parsed_volume);
  }
  if (!has_volume) {
    has_volume = parseVolumePercent(obj["volumePercent"], &parsed_volume);
  }
  if (!has_volume) {
    has_volume = parseVolumePercent(obj["volume_percent"], &parsed_volume);
  }

  bool parsed_muted = false;
  bool has_muted = parseMutedFlag(obj["muted"], &parsed_muted);
  if (!has_muted) {
    has_muted = parseMutedFlag(obj["mute"], &parsed_muted);
  }
  if (!has_muted) {
    has_muted = parseMutedFlag(obj["isMuted"], &parsed_muted);
  }
  if (!has_muted && volume_node.is<JsonObjectConst>()) {
    has_muted = parseMutedFlag(volume_node["muted"], &parsed_muted);
  }
  if (!has_muted && volume_node.is<JsonObjectConst>()) {
    has_muted = parseMutedFlag(volume_node["mute"], &parsed_muted);
  }

  if (!has_volume && !has_muted) {
    return;
  }

  // Accept partial updates: keep previous known value when only one field is present.
  if (!has_volume) {
    parsed_volume = best->valid ? best->volume : 0;
  }
  if (!has_muted) {
    parsed_muted = best->valid ? best->muted : false;
  }

  int32_t parsed_latency = 0;
  bool has_latency = parseIntField(obj["latency"], &parsed_latency);
  if (!has_latency) {
    has_latency = parseIntField(obj["latencyMs"], &parsed_latency);
  }

  int32_t parsed_buffer = 0;
  bool has_buffer = parseIntField(obj["bufferMs"], &parsed_buffer);
  if (!has_buffer) {
    has_buffer = parseIntField(obj["buffer_ms"], &parsed_buffer);
  }

  int score = 10 + identity_score;
  if (has_latency) {
    score += 2;
  }
  if (has_buffer) {
    score += 3;
  }

  if (score > best->score) {
    best->valid = true;
    best->score = score;
    best->volume = parsed_volume;
    best->muted = parsed_muted;
    best->latency = parsed_latency;
    best->buffer_ms = parsed_buffer;
  }
}

void findBestServerSettingsCandidate(JsonVariantConst node,
                                     const char *preferred_client_id,
                                     const char *preferred_client_name,
                                     int inherited_identity_score,
                                     ServerSettingsCandidate *best,
                                     int *visited_candidates) {
  if (!best || node.isNull()) {
    return;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();
    const int local_identity =
        extractIdentityScore(obj, preferred_client_id, preferred_client_name);
    const int effective_identity = inherited_identity_score + local_identity;

    const int prev_score = best->score;
    evaluateServerSettingsObject(obj, effective_identity, best);
    if (visited_candidates && best->score != prev_score) {
      *visited_candidates += 1;
    }

    for (JsonPairConst kv : obj) {
      findBestServerSettingsCandidate(kv.value(), preferred_client_id,
                                      preferred_client_name, effective_identity,
                                      best, visited_candidates);
    }
    return;
  }

  if (node.is<JsonArrayConst>()) {
    for (JsonVariantConst value : node.as<JsonArrayConst>()) {
      findBestServerSettingsCandidate(value, preferred_client_id,
                                      preferred_client_name, inherited_identity_score,
                                      best, visited_candidates);
    }
  }
}

bool parseServerSettingsJson(const uint8_t *json_data, size_t json_len,
                             const char *preferred_client_id,
                             const char *preferred_client_name, uint32_t *volume,
                             bool *muted, int32_t *latency, int32_t *buffer_ms,
                             int *candidate_score, int *candidate_count) {
  if (!json_data || json_len == 0 || !volume || !muted || !latency || !buffer_ms) {
    return false;
  }

  DynamicJsonDocument doc(16384);
  const auto err = deserializeJson(doc, json_data, json_len);
  if (err) {
    LOGE(kTag, "Failed to parse SERVER_SETTINGS JSON: %s", err.c_str());
    return false;
  }

  ServerSettingsCandidate best;
  int visited_candidates = 0;
  findBestServerSettingsCandidate(doc.as<JsonVariantConst>(), preferred_client_id,
                                  preferred_client_name, 0, &best,
                                  &visited_candidates);
  if (!best.valid) {
    return false;
  }

  *volume = best.volume;
  *muted = best.muted;
  *latency = best.latency;
  *buffer_ms = best.buffer_ms;
  if (candidate_score) {
    *candidate_score = best.score;
  }
  if (candidate_count) {
    *candidate_count = visited_candidates;
  }
  return true;
}

bool parseCodecHeaderPcm(const uint8_t *payload, size_t payload_size, uint32_t *sample_rate_hz,
                         uint16_t *bits_per_sample, uint16_t *channels) {
  if (!payload || payload_size < 36 || !sample_rate_hz || !bits_per_sample || !channels) {
    return false;
  }

  *channels = static_cast<uint16_t>(payload[22] | (payload[23] << 8));
  *sample_rate_hz = readLe32(payload + 24);
  *bits_per_sample = static_cast<uint16_t>(payload[34] | (payload[35] << 8));
  return true;
}
}

bool SnapcastClient::connect(const char *host, uint16_t port) {
  if (client_.connected()) {
    return true;
  }

  client_.setTimeout(1000);
  if (!client_.connect(host, port)) {
    return false;
  }

  LOGI(kTag, "TCP connected to %s:%u", host, port);
  return sendHelloMessage();
}

void SnapcastClient::disconnect() {
  if (client_.connected()) {
    client_.stop();
    LOGI(kTag, "TCP disconnected");
  }

  resetRxState();
}

void SnapcastClient::setSyncOffsetCallback(SyncOffsetCallback callback) {
  sync_offset_callback_ = callback;
}

bool SnapcastClient::getSyncOffset(int64_t *out_us) const {
  if (!out_us || !sync_ready_) {
    return false;
  }
  *out_us = current_offset_us_;
  return true;
}

void SnapcastClient::insertOffsetSample(int64_t offset_us) {
  constexpr int64_t kMaxOffsetJumpUs = 180000LL;
  constexpr uint8_t kOutlierConfirmCount = 3;

  if (sync_ready_) {
    const int64_t delta_us = llabs(offset_us - current_offset_us_);
    if (delta_us > kMaxOffsetJumpUs) {
      if (offset_outlier_streak_ < 255) {
        ++offset_outlier_streak_;
      }

      if (offset_outlier_streak_ < kOutlierConfirmCount) {
        LOGW(kTag,
             "TIME outlier ignored: raw=%lldus current=%lldus delta=%lldus (streak=%u/%u)",
             static_cast<long long>(offset_us),
             static_cast<long long>(current_offset_us_),
             static_cast<long long>(delta_us),
             static_cast<unsigned>(offset_outlier_streak_),
             static_cast<unsigned>(kOutlierConfirmCount));
        return;
      }

      LOGW(kTag,
           "TIME offset jump accepted after %u consecutive samples: %lldus -> %lldus",
           static_cast<unsigned>(offset_outlier_streak_),
           static_cast<long long>(current_offset_us_),
           static_cast<long long>(offset_us));

      // Hard re-anchor the median filter on confirmed new baseline.
      for (int i = 0; i < kOffsetFilterSize; ++i) {
        offset_samples_[i] = offset_us;
      }
      offset_sample_count_ = kOffsetFilterSize;
      offset_write_idx_ = 0;
      current_offset_us_ = offset_us;
      offset_outlier_streak_ = 0;
      if (sync_offset_callback_) {
        sync_offset_callback_(current_offset_us_);
      }
      return;
    }

    offset_outlier_streak_ = 0;
  }

  offset_samples_[offset_write_idx_] = offset_us;
  offset_write_idx_ = (offset_write_idx_ + 1) % kOffsetFilterSize;
  if (offset_sample_count_ < kOffsetFilterSize) {
    ++offset_sample_count_;
  }

  if (offset_sample_count_ >= kOffsetFilterSize) {
    // Compute median from a sorted copy
    int64_t sorted[kOffsetFilterSize];
    for (int i = 0; i < kOffsetFilterSize; ++i) {
      sorted[i] = offset_samples_[i];
    }
    std::sort(sorted, sorted + kOffsetFilterSize);
    current_offset_us_ = sorted[kOffsetFilterSize / 2];
    sync_ready_ = true;
    if (sync_offset_callback_) {
      sync_offset_callback_(current_offset_us_);
    }
    LOGD(kTag, "TIME sync offset=%lldus (median of %d samples)",
         static_cast<long long>(current_offset_us_), kOffsetFilterSize);
  }
}

void SnapcastClient::tick() {
  if (!client_.connected()) {
    return;
  }

  const uint32_t now_ms = millis();
  const uint32_t period =
      (offset_sample_count_ < kOffsetFilterSize) ? kTimeFastPeriodMs : kTimeSlowPeriodMs;
  if (now_ms - last_time_message_ms_ >= period) {
    if (sendTimeMessage()) {
      last_time_message_ms_ = now_ms;
    }
  }

  if (!readIncoming()) {
    LOGE(kTag, "Snapcast stream parse error, disconnecting");
    disconnect();
  }
}

bool SnapcastClient::isConnected() { return client_.connected(); }

void SnapcastClient::setServerSettingsCallback(ServerSettingsCallback callback) {
  server_settings_callback_ = callback;
}

void SnapcastClient::setCodecHeaderPcmCallback(CodecHeaderPcmCallback callback) {
  codec_header_pcm_callback_ = callback;
}

void SnapcastClient::setWireChunkPcmCallback(WireChunkPcmCallback callback) {
  wire_chunk_pcm_callback_ = callback;
}

void SnapcastClient::setClientName(const char *name) {
  if (!name || name[0] == '\0') {
    strncpy(client_name_, kAppConfig.network.client_name, sizeof(client_name_) - 1);
    client_name_[sizeof(client_name_) - 1] = '\0';
    return;
  }

  strncpy(client_name_, name, sizeof(client_name_) - 1);
  client_name_[sizeof(client_name_) - 1] = '\0';
}

const char *SnapcastClient::clientName() const {
  if (client_name_[0] == '\0') {
    return kAppConfig.network.client_name;
  }
  return client_name_;
}

bool SnapcastClient::sendHelloMessage() {
  String mac = WiFi.macAddress();
  // Use the user-configured device name as HostName so the Snapcast server
  // displays the correct friendly name instead of the default WiFi hostname.
  String host = String(clientName());
  if (host.isEmpty()) {
    host = WiFi.getHostname();
  }
  if (host.isEmpty()) {
    host = "esp32";
  }

  char id_buf[48];
  snprintf(id_buf, sizeof(id_buf), "%s-1", mac.c_str());
  strncpy(client_id_, id_buf, sizeof(client_id_) - 1);
  client_id_[sizeof(client_id_) - 1] = '\0';

  char json[512];
  const int json_len = snprintf(
      json, sizeof(json),
      "{\"MAC\":\"%s\",\"HostName\":\"%s\",\"Version\":\"%s\","
      "\"ClientName\":\"%s\",\"OS\":\"%s\",\"Arch\":\"%s\","
      "\"Instance\":1,\"ID\":\"%s\",\"SnapStreamProtocolVersion\":%u}",
      mac.c_str(), host.c_str(), kAppConfig.network.client_version,
      clientName(), kAppConfig.network.client_os,
      kAppConfig.network.client_arch, id_buf,
      static_cast<unsigned>(kAppConfig.network.protocol_version));

  if (json_len <= 0 || static_cast<size_t>(json_len) >= sizeof(json)) {
    LOGE(kTag, "Failed to build hello JSON payload");
    return false;
  }

  const uint32_t payload_len = static_cast<uint32_t>(json_len + 4);
  uint8_t base[SnapcastClient::kBaseMessageSize] = {0};
  const int64_t now_us = nowUs();

  writeLe16(&base[0], kSnapcastMessageHello);
  writeLe16(&base[2], message_id_++);
  writeLe16(&base[4], kSnapcastRefersToNone);
  writeLe32(&base[6],  static_cast<uint32_t>(now_us / 1000000LL));
  writeLe32(&base[10], static_cast<uint32_t>(now_us % 1000000LL));
  writeLe32(&base[14], 0);
  writeLe32(&base[18], 0);
  writeLe32(&base[22], payload_len);

  uint8_t json_prefix[4];
  writeLe32(json_prefix, static_cast<uint32_t>(json_len));

  if (!writeAll(base, sizeof(base)) || !writeAll(json_prefix, sizeof(json_prefix)) ||
      !writeAll(reinterpret_cast<const uint8_t *>(json), static_cast<size_t>(json_len))) {
    LOGE(kTag, "Failed to send hello message");
    return false;
  }

  LOGI(kTag, "Snapcast HELLO sent (id=%u, payload=%u bytes)",
       static_cast<unsigned>(message_id_ - 1), static_cast<unsigned>(payload_len));
  return true;
}

bool SnapcastClient::sendTimeMessage() {
  uint8_t base[SnapcastClient::kBaseMessageSize] = {0};
  uint8_t payload[8] = {0};  // tv_t latency: zero on outgoing, server echoes via base headers

  // Record T1 (client send time) for RTT computation on response
  time_sent_us_ = nowUs();
  time_sent_id_ = message_id_;

  writeLe16(&base[0], kSnapcastMessageTime);
  writeLe16(&base[2], message_id_++);
  writeLe16(&base[4], kSnapcastRefersToNone);
  writeLe32(&base[6],  static_cast<uint32_t>(time_sent_us_ / 1000000LL));
  writeLe32(&base[10], static_cast<uint32_t>(time_sent_us_ % 1000000LL));
  writeLe32(&base[14], 0);
  writeLe32(&base[18], 0);
  writeLe32(&base[22], sizeof(payload));

  // Payload latency = 0: server will compute T2_server - T1_client and echo via payload
  writeLe32(&payload[0], 0);
  writeLe32(&payload[4], 0);

  if (!writeAll(base, sizeof(base)) || !writeAll(payload, sizeof(payload))) {
    LOGE(kTag, "Failed to send TIME message");
    return false;
  }

  LOGD(kTag, "TIME sent id=%u T1=%lldus", static_cast<unsigned>(message_id_ - 1),
       static_cast<long long>(time_sent_us_));
  return true;
}

bool SnapcastClient::writeAll(const uint8_t *data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const size_t chunk = client_.write(data + sent, size - sent);
    if (chunk == 0) {
      return false;
    }
    sent += chunk;
  }
  return true;
}

bool SnapcastClient::readExact(uint8_t *buffer, size_t target_size, size_t *bytes_read) {
  if (!buffer || !bytes_read) {
    return false;
  }

  while (*bytes_read < target_size) {
    const int available = client_.available();
    if (available <= 0) {
      return true;
    }

    const size_t remaining = target_size - *bytes_read;
    const size_t to_read = (static_cast<size_t>(available) < remaining)
                               ? static_cast<size_t>(available)
                               : remaining;
    const int read_count = client_.read(buffer + *bytes_read, to_read);
    if (read_count < 0) {
      return false;
    }
    if (read_count == 0) {
      return true;
    }
    *bytes_read += static_cast<size_t>(read_count);
  }

  return true;
}

bool SnapcastClient::readIncoming() {
  while (client_.available() > 0) {
    if (!has_header_) {
      if (!readExact(rx_header_.data(), kBaseMessageSize, &rx_header_read_)) {
        return false;
      }

      if (rx_header_read_ < kBaseMessageSize) {
        return true;
      }

      if (!tryDecodeHeader(&current_header_)) {
        return false;
      }

      if (current_header_.payload_size > kMaxPayloadSize) {
        LOGE(kTag, "Payload too large: %u", static_cast<unsigned>(current_header_.payload_size));
        return false;
      }

      rx_payload_.assign(current_header_.payload_size, 0);
      rx_payload_read_ = 0;
      has_header_ = true;
    }

    if (!rx_payload_.empty()) {
      if (!readExact(rx_payload_.data(), rx_payload_.size(), &rx_payload_read_)) {
        return false;
      }
    }

    if (rx_payload_read_ < rx_payload_.size()) {
      return true;
    }

    if (!handleCompleteMessage(current_header_, rx_payload_.data(), rx_payload_.size())) {
      return false;
    }

    resetRxState();
  }

  return true;
}

bool SnapcastClient::handleCompleteMessage(const BaseMessage &header, const uint8_t *payload,
                                           size_t payload_size) {
  switch (header.type) {
    case kSnapcastMessageCodecHeader:
      LOGI(kTag, "RX CODEC_HEADER id=%u payload=%u", static_cast<unsigned>(header.id),
           static_cast<unsigned>(payload_size));

      if (payload_size < 8) {
        return false;
      }

      {
        const uint32_t codec_name_len = readLe32(payload);
        if (codec_name_len == 0 || codec_name_len + 8 > payload_size) {
          LOGE(kTag, "Invalid CODEC_HEADER codec length: %u",
               static_cast<unsigned>(codec_name_len));
          return false;
        }

        char codec_name[16] = {0};
        const size_t codec_copy_len =
            (codec_name_len < sizeof(codec_name) - 1) ? codec_name_len : sizeof(codec_name) - 1;
        memcpy(codec_name, payload + 4, codec_copy_len);
        codec_name[codec_copy_len] = '\0';

        const uint32_t codec_payload_len = readLe32(payload + 4 + codec_name_len);
        const uint8_t *codec_payload = payload + 8 + codec_name_len;
        if (8 + codec_name_len + codec_payload_len > payload_size) {
          LOGE(kTag, "Invalid CODEC_HEADER payload length: %u",
               static_cast<unsigned>(codec_payload_len));
          return false;
        }

        current_codec_is_pcm_ = (strcmp(codec_name, "pcm") == 0);
        LOGI(kTag, "Codec selected: %s (payload=%u)", codec_name,
             static_cast<unsigned>(codec_payload_len));

        if (current_codec_is_pcm_) {
          uint32_t sample_rate_hz = 0;
          uint16_t bits_per_sample = 0;
          uint16_t channels = 0;
          if (parseCodecHeaderPcm(codec_payload, codec_payload_len, &sample_rate_hz,
                                  &bits_per_sample, &channels)) {
            current_sample_rate_hz_ = sample_rate_hz;
            current_bits_per_sample_ = bits_per_sample;
            current_channels_ = channels;
            LOGI(kTag, "PCM format %lu:%u:%u", static_cast<unsigned long>(sample_rate_hz),
                 static_cast<unsigned>(bits_per_sample), static_cast<unsigned>(channels));
            if (codec_header_pcm_callback_) {
              codec_header_pcm_callback_(sample_rate_hz, bits_per_sample, channels);
            }
          } else {
            LOGE(kTag, "Failed to parse PCM codec header payload");
          }
        }
      }
      break;
    case kSnapcastMessageWireChunk:
      if (current_codec_is_pcm_) {
        if (payload_size < 12) {
          return false;
        }

        const uint32_t chunk_size = readLe32(payload + 8);
        const uint8_t *chunk_payload = payload + 12;
        const size_t available = payload_size - 12;
        if (chunk_size > available) {
          LOGE(kTag, "WIRE_CHUNK invalid chunk size: %u > %u",
               static_cast<unsigned>(chunk_size), static_cast<unsigned>(available));
          return false;
        }

        ++wire_chunk_count_;
        wire_chunk_bytes_ += chunk_size;
        const uint32_t now_ms = millis();
        if (last_wire_chunk_stats_ms_ == 0) {
          last_wire_chunk_stats_ms_ = now_ms;
        } else if (now_ms - last_wire_chunk_stats_ms_ >= 1000) {
          LOGD(kTag, "PCM throughput: chunks=%lu bytes=%lu/s queued_format=%lu:%u:%u",
               static_cast<unsigned long>(wire_chunk_count_),
               static_cast<unsigned long>(wire_chunk_bytes_),
               static_cast<unsigned long>(current_sample_rate_hz_),
               static_cast<unsigned>(current_bits_per_sample_),
               static_cast<unsigned>(current_channels_));
          wire_chunk_count_ = 0;
          wire_chunk_bytes_ = 0;
          last_wire_chunk_stats_ms_ = now_ms;
        }

        if (wire_chunk_pcm_callback_) {
          const int32_t ts_sec  = static_cast<int32_t>(readLe32(payload));
          const int32_t ts_usec = static_cast<int32_t>(readLe32(payload + 4));
          const int64_t chunk_ts_us =
              static_cast<int64_t>(ts_sec) * 1000000LL + static_cast<int64_t>(ts_usec);
          wire_chunk_pcm_callback_(chunk_payload, chunk_size, chunk_ts_us);
        }
      }
      break;
    case kSnapcastMessageServerSettings:
      LOGI(kTag, "RX SERVER_SETTINGS id=%u payload=%u", static_cast<unsigned>(header.id),
           static_cast<unsigned>(payload_size));

      if (payload_size < 4) {
        return false;
      }

      {
        const uint32_t declared_len = readLe32(payload);
        if (declared_len == 0 || declared_len > payload_size - 4) {
          LOGE(kTag, "SERVER_SETTINGS invalid length prefix: %u",
               static_cast<unsigned>(declared_len));
          return false;
        }

        uint32_t volume = 0;
        bool muted = false;
        int32_t latency = 0;
        int32_t buffer_ms = 0;

        int candidate_score = -1;
        int candidate_count = 0;
        if (parseServerSettingsJson(payload + 4, declared_len, client_id_,
                  clientName(), &volume, &muted, &latency,
                  &buffer_ms, &candidate_score,
                  &candidate_count)) {
          LOGI(kTag,
               "SERVER_SETTINGS parsed: volume=%u muted=%s latency=%ld bufferMs=%ld (score=%d candidates=%d client='%s' id='%s')",
               static_cast<unsigned>(volume), muted ? "true" : "false",
               static_cast<long>(latency), static_cast<long>(buffer_ms),
               candidate_score, candidate_count, clientName(), client_id_);

          if (server_settings_callback_) {
            server_settings_callback_(volume, muted, latency, buffer_ms);
          }
        } else {
          LOGE(kTag, "Failed to parse SERVER_SETTINGS JSON");
        }
      }
      break;
    case kSnapcastMessageTime: {
      // NTP-style offset computation (see IDF main.c time_sync_msg_received).
      // T1 = time_sent_us_ (client clock when we sent our TIME request)
      // T3 = server sent time (server clock), from base header .sent
      // T4 = client receive time (client clock), sampled now
      // payload[0..7] = T2_server - T1_client (server latency, from server)
      // offset = ((T2 - T1) + (T3 - T4)) / 2  =>  server_clock - client_clock
      if (payload_size >= 8 && time_sent_us_ != 0) {
        const bool matches_last_time_request =
            (header.id == time_sent_id_) || (header.refers_to == time_sent_id_);
        if (!matches_last_time_request) {
          LOGD(kTag,
               "RX TIME ignored (stale): id=%u refers_to=%u last_sent_id=%u",
               static_cast<unsigned>(header.id),
               static_cast<unsigned>(header.refers_to),
               static_cast<unsigned>(time_sent_id_));
          break;
        }

        const int64_t t4_us = nowUs();  // client receive time
        const int64_t t3_us =
            static_cast<int64_t>(header.sent_sec) * 1000000LL +
            static_cast<int64_t>(header.sent_usec);  // server sent time
        // payload contains T2_server - T1_client as tv_t (int32 sec + int32 usec)
        const int32_t lat_sec  = static_cast<int32_t>(readLe32(payload));
        const int32_t lat_usec = static_cast<int32_t>(readLe32(payload + 4));
        const int64_t t2_minus_t1 =
            static_cast<int64_t>(lat_sec) * 1000000LL + static_cast<int64_t>(lat_usec);
        const int64_t offset_us = (t2_minus_t1 + (t3_us - t4_us)) / 2;
        insertOffsetSample(offset_us);
        LOGD(kTag,
             "RX TIME id=%u t2-t1=%lldus t3-t4=%lldus offset=%lldus ready=%d",
             static_cast<unsigned>(header.id),
             static_cast<long long>(t2_minus_t1),
             static_cast<long long>(t3_us - t4_us),
             static_cast<long long>(offset_us),
             static_cast<int>(sync_ready_));
      } else {
        LOGD(kTag, "RX TIME id=%u (no T1 or short payload)",
             static_cast<unsigned>(header.id));
      }
      break;
    }
    case kSnapcastMessageHello:
      LOGI(kTag, "RX HELLO id=%u payload=%u", static_cast<unsigned>(header.id),
           static_cast<unsigned>(payload_size));
      break;
    case kSnapcastMessageClientInfo:
      LOGI(kTag, "RX CLIENT_INFO id=%u payload=%u", static_cast<unsigned>(header.id),
           static_cast<unsigned>(payload_size));
      break;
    default:
      LOGI(kTag, "RX type=%u id=%u payload=%u", static_cast<unsigned>(header.type),
           static_cast<unsigned>(header.id), static_cast<unsigned>(payload_size));
      break;
  }

  return true;
}

bool SnapcastClient::tryDecodeHeader(BaseMessage *out_header) const {
  if (!out_header) {
    return false;
  }

  out_header->type = readLe16(&rx_header_[0]);
  out_header->id = readLe16(&rx_header_[2]);
  out_header->refers_to = readLe16(&rx_header_[4]);
  out_header->sent_sec = static_cast<int32_t>(readLe32(&rx_header_[6]));
  out_header->sent_usec = static_cast<int32_t>(readLe32(&rx_header_[10]));
  out_header->received_sec = static_cast<int32_t>(readLe32(&rx_header_[14]));
  out_header->received_usec = static_cast<int32_t>(readLe32(&rx_header_[18]));
  out_header->payload_size = readLe32(&rx_header_[22]);

  return true;
}

void SnapcastClient::resetRxState() {
  rx_header_read_ = 0;
  rx_payload_.clear();
  rx_payload_read_ = 0;
  current_header_ = {};
  has_header_ = false;
}

}  // namespace snapcast
