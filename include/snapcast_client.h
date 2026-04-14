#pragma once

#include <WiFiClient.h>
#include <esp_timer.h>

#include <array>
#include <stdint.h>
#include <vector>

namespace snapcast {

class SnapcastClient {
 public:
  using ServerSettingsCallback =
      void (*)(uint32_t volume_percent, bool muted, int32_t latency_ms,
               int32_t buffer_ms);
  using CodecHeaderPcmCallback =
      void (*)(uint32_t sample_rate_hz, uint16_t bits_per_sample, uint16_t channels);
  // chunk_ts_us: server-clock timestamp of this chunk in microseconds
  using WireChunkPcmCallback =
      void (*)(const uint8_t *payload, size_t size, int64_t chunk_ts_us);
  // Called each time the sync offset is updated (server_clock - client_clock, µs)
  using SyncOffsetCallback = void (*)(int64_t offset_us);

  bool connect(const char *host, uint16_t port);
  void disconnect();
  void tick();
  bool isConnected();

  // Returns true and writes the latest estimated server clock offset when sync
  // is ready (enough TIME round-trips collected).
  bool getSyncOffset(int64_t *out_us) const;

  void setServerSettingsCallback(ServerSettingsCallback callback);
  void setCodecHeaderPcmCallback(CodecHeaderPcmCallback callback);
  void setWireChunkPcmCallback(WireChunkPcmCallback callback);
  void setSyncOffsetCallback(SyncOffsetCallback callback);
  void setClientName(const char *name);
  const char *clientName() const;

 private:
  struct BaseMessage {
    uint16_t type;
    uint16_t id;
    uint16_t refers_to;
    int32_t sent_sec;
    int32_t sent_usec;
    int32_t received_sec;
    int32_t received_usec;
    uint32_t payload_size;
  };

  bool sendHelloMessage();
  bool sendTimeMessage();
  bool writeAll(const uint8_t *data, size_t size);
  bool readExact(uint8_t *buffer, size_t target_size, size_t *bytes_read);
  bool readIncoming();
  bool handleCompleteMessage(const BaseMessage &header, const uint8_t *payload,
                             size_t payload_size);
  bool tryDecodeHeader(BaseMessage *out_header) const;
  void resetRxState();
  void insertOffsetSample(int64_t offset_us);

  static constexpr size_t kBaseMessageSize = 26;
  static constexpr uint32_t kMaxPayloadSize = 64 * 1024;
  // Fast period until sync filter is full; slow period thereafter
  static constexpr uint32_t kTimeFastPeriodMs = 500;
  static constexpr uint32_t kTimeSlowPeriodMs = 5000;
  // Number of TIME round-trips before sync is considered ready
  static constexpr int kOffsetFilterSize = 16;

  uint16_t message_id_ = 1;
  uint32_t last_time_message_ms_ = 0;
  ServerSettingsCallback server_settings_callback_ = nullptr;
  CodecHeaderPcmCallback codec_header_pcm_callback_ = nullptr;
  WireChunkPcmCallback wire_chunk_pcm_callback_ = nullptr;
  SyncOffsetCallback sync_offset_callback_ = nullptr;
  bool current_codec_is_pcm_ = false;
  uint32_t current_sample_rate_hz_ = 44100;
  uint16_t current_bits_per_sample_ = 16;
  uint16_t current_channels_ = 2;
  std::array<uint8_t, kBaseMessageSize> rx_header_{};
  size_t rx_header_read_ = 0;
  std::vector<uint8_t> rx_payload_;
  size_t rx_payload_read_ = 0;
  BaseMessage current_header_{};
  bool has_header_ = false;
  uint32_t wire_chunk_count_ = 0;
  uint32_t wire_chunk_bytes_ = 0;
  uint32_t last_wire_chunk_stats_ms_ = 0;
  WiFiClient client_;

  // TIME sync state
  int64_t time_sent_us_ = 0;     // esp_timer_get_time() when last TIME was sent
  uint16_t time_sent_id_ = 0;    // message_id of last sent TIME message
  int64_t offset_samples_[kOffsetFilterSize] = {};  // circular buffer of raw offset measurements
  int offset_write_idx_ = 0;     // next write position in offset_samples_
  int offset_sample_count_ = 0;  // number of samples collected (capped at kOffsetFilterSize)
  bool sync_ready_ = false;
  int64_t current_offset_us_ = 0;  // latest median offset (server_clock - client_clock)
  uint8_t offset_outlier_streak_ = 0;
  char client_name_[33] = {0};
  char client_id_[48] = {0};
};

}  // namespace snapcast
