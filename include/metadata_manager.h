#pragma once

#include <Arduino.h>
#include <WiFiClient.h>

namespace snapcast {

class MetadataManager {
 public:
  bool begin();
  void tick();

  void setTarget(const char *host, uint16_t stream_port);
  bool isPlaying() const;
  const String &title() const;
  const String &artist() const;
  const String &album() const;

 private:
  bool pollServerStatus();
  bool parseServerStatusFrame(const String &json_line);

  String host_;
  uint16_t stream_port_ = 1704;
  uint16_t control_port_ = 1705;
  uint32_t last_poll_ms_ = 0;
  bool playing_ = false;
  String title_;
  String artist_;
  String album_;
  WiFiClient client_;
};

}  // namespace snapcast
