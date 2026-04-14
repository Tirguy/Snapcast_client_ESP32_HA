#pragma once

#include <Arduino.h>

namespace snapcast {

enum class EqPreset : uint8_t {
  kNormal = 0,
  kBassBoost = 1,
  kTrebleBoost = 2,
  kBright = 3,
  kCustom = 4,
};

struct RuntimeNetworkSettings {
  char wifi_ssid[64];
  char wifi_password[64];
  char snapcast_host[64];
  char device_name[33];
  uint16_t snapcast_port;
  int16_t stream_trim_ms;
  EqPreset eq_preset;
  int8_t eq_bass_gain_db;
  int8_t eq_treble_gain_db;
  uint16_t eq_bass_freq_hz;
  uint16_t eq_treble_freq_hz;
  bool loaded_from_file;
};

struct VoiceSettings {
  char ha_host[128];
  int32_t ha_port;
  char ha_token[512];
  char ha_pipeline_id[128];
};

class StorageService {
 public:
  bool begin();
  uint32_t incrementBootCount();
  uint32_t bootCount() const;
  const RuntimeNetworkSettings &networkSettings() const;
  bool saveNetworkSettings(const RuntimeNetworkSettings &settings);
  bool resetWifiSettings();
  const VoiceSettings &voiceSettings() const;
  bool saveVoiceSettings(const VoiceSettings &settings);
  bool resetVoiceParam(const char *param_name);

 private:
  void applyDefaultNetworkSettings();
  bool loadNetworkSettings();
  void loadVoiceSettings();

  uint32_t boot_count_ = 0;
  RuntimeNetworkSettings network_settings_{};
  VoiceSettings voice_settings_{};
};

}  // namespace snapcast
