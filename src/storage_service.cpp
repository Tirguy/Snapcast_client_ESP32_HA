#include "storage_service.h"

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "storage";
constexpr const char *kBootCountPath = "/boot_count.txt";
constexpr const char *kNetworkConfigPath = "/network_config.txt";
constexpr const char *kVoiceConfigPath = "/voice_config.txt";
constexpr const char *kDefaultDeviceNamePrefix = "HIFI-ESP32-";
constexpr int8_t kEqGainMinDb = -12;
constexpr int8_t kEqGainMaxDb = 12;
constexpr uint16_t kEqBassFreqMinHz = 60;
constexpr uint16_t kEqBassFreqMaxHz = 400;
constexpr uint16_t kEqTrebleFreqMinHz = 2000;
constexpr uint16_t kEqTrebleFreqMaxHz = 12000;

void copyString(char *destination, size_t destination_size, const char *source) {
  if (!destination || destination_size == 0) {
    return;
  }

  if (!source) {
    destination[0] = '\0';
    return;
  }

  strncpy(destination, source, destination_size - 1);
  destination[destination_size - 1] = '\0';
}

void trimInPlace(char *text) {
  if (!text) {
    return;
  }

  size_t start = 0;
  while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') {
    ++start;
  }

  if (start > 0) {
    memmove(text, text + start, strlen(text + start) + 1);
  }

  size_t length = strlen(text);
  while (length > 0) {
    const char tail = text[length - 1];
    if (tail != ' ' && tail != '\t' && tail != '\r' && tail != '\n') {
      break;
    }
    text[length - 1] = '\0';
    --length;
  }
}

void buildDefaultDeviceName(char *name, size_t size) {
  if (!name || size == 0) {
    return;
  }

  const uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFF);
  char generated[33] = {0};
  snprintf(generated, sizeof(generated), "%s%06lX", kDefaultDeviceNamePrefix,
           static_cast<unsigned long>(chip));
  copyString(name, size, generated);
}

bool isAllowedDeviceNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
}

void sanitizeDeviceNameInPlace(char *name, size_t size) {
  if (!name || size == 0) {
    return;
  }

  char sanitized[33] = {0};
  size_t out = 0;
  bool last_was_space = false;

  for (size_t i = 0; name[i] != '\0' && out < sizeof(sanitized) - 1; ++i) {
    const char c = name[i];
    if (!isAllowedDeviceNameChar(c)) {
      continue;
    }

    if (c == ' ') {
      if (out == 0 || last_was_space) {
        continue;
      }
      last_was_space = true;
      sanitized[out++] = c;
      continue;
    }

    last_was_space = false;
    sanitized[out++] = c;
  }

  while (out > 0 && sanitized[out - 1] == ' ') {
    --out;
  }
  sanitized[out] = '\0';

  if (out == 0) {
    buildDefaultDeviceName(sanitized, sizeof(sanitized));
  }

  copyString(name, size, sanitized);
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

void applyEqPresetDefaults(EqPreset preset, RuntimeNetworkSettings *settings) {
  if (!settings) {
    return;
  }

  switch (preset) {
    case EqPreset::kNormal:
      settings->eq_bass_gain_db = 0;
      settings->eq_treble_gain_db = 0;
      settings->eq_bass_freq_hz = 120;
      settings->eq_treble_freq_hz = 6000;
      break;
    case EqPreset::kBassBoost:
      settings->eq_bass_gain_db = 6;
      settings->eq_treble_gain_db = 0;
      settings->eq_bass_freq_hz = 120;
      settings->eq_treble_freq_hz = 6000;
      break;
    case EqPreset::kTrebleBoost:
      settings->eq_bass_gain_db = 0;
      settings->eq_treble_gain_db = 6;
      settings->eq_bass_freq_hz = 120;
      settings->eq_treble_freq_hz = 7000;
      break;
    case EqPreset::kBright:
      settings->eq_bass_gain_db = -2;
      settings->eq_treble_gain_db = 4;
      settings->eq_bass_freq_hz = 140;
      settings->eq_treble_freq_hz = 8000;
      break;
    case EqPreset::kCustom:
      break;
  }
}

void normalizeEqSettings(RuntimeNetworkSettings *settings) {
  if (!settings) {
    return;
  }

  if (static_cast<uint8_t>(settings->eq_preset) > static_cast<uint8_t>(EqPreset::kCustom)) {
    settings->eq_preset = EqPreset::kNormal;
  }

  if (settings->eq_preset != EqPreset::kCustom) {
    applyEqPresetDefaults(settings->eq_preset, settings);
  }

  settings->eq_bass_gain_db =
      clampValue<int8_t>(settings->eq_bass_gain_db, kEqGainMinDb, kEqGainMaxDb);
  settings->eq_treble_gain_db =
      clampValue<int8_t>(settings->eq_treble_gain_db, kEqGainMinDb, kEqGainMaxDb);
  settings->eq_bass_freq_hz =
      clampValue<uint16_t>(settings->eq_bass_freq_hz, kEqBassFreqMinHz, kEqBassFreqMaxHz);
  settings->eq_treble_freq_hz = clampValue<uint16_t>(settings->eq_treble_freq_hz,
                                                     kEqTrebleFreqMinHz,
                                                     kEqTrebleFreqMaxHz);
}
}

bool StorageService::begin() {
  if (SPIFFS.begin(false)) {
    LOGI(kTag, "SPIFFS mounted");
    loadVoiceSettings();
    return loadNetworkSettings();
  }

  LOGI(kTag, "SPIFFS mount failed, trying format");
  if (!SPIFFS.begin(true)) {
    return false;
  }

  LOGI(kTag, "SPIFFS formatted and mounted");
  loadVoiceSettings();
  return loadNetworkSettings();
}

uint32_t StorageService::incrementBootCount() {
  File file = SPIFFS.open(kBootCountPath, FILE_READ);
  if (file) {
    boot_count_ = static_cast<uint32_t>(file.parseInt());
    file.close();
  }

  ++boot_count_;

  file = SPIFFS.open(kBootCountPath, FILE_WRITE, true);
  if (file) {
    file.print(boot_count_);
    file.close();
  }

  return boot_count_;
}

uint32_t StorageService::bootCount() const { return boot_count_; }

const RuntimeNetworkSettings &StorageService::networkSettings() const {
  return network_settings_;
}

bool StorageService::saveNetworkSettings(const RuntimeNetworkSettings &settings) {
  RuntimeNetworkSettings normalized = settings;
  sanitizeDeviceNameInPlace(normalized.device_name, sizeof(normalized.device_name));
  normalizeEqSettings(&normalized);

  File file = SPIFFS.open(kNetworkConfigPath, FILE_WRITE, true);
  if (!file) {
    LOGE(kTag, "Failed to open %s for writing", kNetworkConfigPath);
    return false;
  }

  file.printf("wifi_ssid=%s\n", normalized.wifi_ssid);
  file.printf("wifi_password=%s\n", normalized.wifi_password);
  file.printf("snapcast_host=%s\n", normalized.snapcast_host);
  file.printf("snapcast_port=%u\n", static_cast<unsigned>(normalized.snapcast_port));
  file.printf("stream_trim_ms=%d\n", static_cast<int>(normalized.stream_trim_ms));
  file.printf("device_name=%s\n", normalized.device_name);
  file.printf("eq_preset=%u\n", static_cast<unsigned>(normalized.eq_preset));
  file.printf("eq_bass_gain_db=%d\n", static_cast<int>(normalized.eq_bass_gain_db));
  file.printf("eq_treble_gain_db=%d\n", static_cast<int>(normalized.eq_treble_gain_db));
  file.printf("eq_bass_freq_hz=%u\n", static_cast<unsigned>(normalized.eq_bass_freq_hz));
  file.printf("eq_treble_freq_hz=%u\n", static_cast<unsigned>(normalized.eq_treble_freq_hz));
  file.close();

  network_settings_ = normalized;
  network_settings_.loaded_from_file = true;
  LOGI(kTag, "Network settings saved to %s", kNetworkConfigPath);
  return true;
}

bool StorageService::resetWifiSettings() {
  RuntimeNetworkSettings cleared = network_settings_;
  cleared.wifi_ssid[0] = '\0';
  cleared.wifi_password[0] = '\0';
  const bool saved = saveNetworkSettings(cleared);
  if (saved) {
    LOGW(kTag, "WiFi credentials cleared from %s", kNetworkConfigPath);
  }
  return saved;
}

void StorageService::applyDefaultNetworkSettings() {
  copyString(network_settings_.wifi_ssid, sizeof(network_settings_.wifi_ssid),
             kAppConfig.network.wifi_ssid);
  copyString(network_settings_.wifi_password, sizeof(network_settings_.wifi_password),
             kAppConfig.network.wifi_password);
  copyString(network_settings_.snapcast_host, sizeof(network_settings_.snapcast_host),
             kAppConfig.network.snapcast_host);
  buildDefaultDeviceName(network_settings_.device_name, sizeof(network_settings_.device_name));
  sanitizeDeviceNameInPlace(network_settings_.device_name, sizeof(network_settings_.device_name));
  network_settings_.snapcast_port = kAppConfig.network.snapcast_port;
  network_settings_.stream_trim_ms = 0;
  network_settings_.eq_preset = EqPreset::kNormal;
  applyEqPresetDefaults(network_settings_.eq_preset, &network_settings_);
  network_settings_.loaded_from_file = false;
}

bool StorageService::loadNetworkSettings() {
  applyDefaultNetworkSettings();

  File file = SPIFFS.open(kNetworkConfigPath, FILE_READ);
  if (!file) {
    LOGI(kTag, "No network config found, creating default %s", kNetworkConfigPath);
    return saveNetworkSettings(network_settings_);
  }

  RuntimeNetworkSettings loaded = network_settings_;
  loaded.loaded_from_file = true;
  bool device_name_loaded = false;

  while (file.available()) {
    String raw_line = file.readStringUntil('\n');
    raw_line.trim();
    if (raw_line.isEmpty() || raw_line.startsWith("#")) {
      continue;
    }

    const int separator = raw_line.indexOf('=');
    if (separator <= 0) {
      continue;
    }

    String key = raw_line.substring(0, separator);
    String value = raw_line.substring(separator + 1);
    key.trim();
    value.trim();

    if (key == "wifi_ssid") {
      copyString(loaded.wifi_ssid, sizeof(loaded.wifi_ssid), value.c_str());
    } else if (key == "wifi_password") {
      copyString(loaded.wifi_password, sizeof(loaded.wifi_password), value.c_str());
    } else if (key == "snapcast_host") {
      copyString(loaded.snapcast_host, sizeof(loaded.snapcast_host), value.c_str());
    } else if (key == "snapcast_port") {
      const long parsed = value.toInt();
      if (parsed > 0 && parsed <= 65535) {
        loaded.snapcast_port = static_cast<uint16_t>(parsed);
      }
    } else if (key == "stream_trim_ms") {
      long parsed = value.toInt();
      if (parsed < -2000) {
        parsed = -2000;
      }
      if (parsed > 2000) {
        parsed = 2000;
      }
      loaded.stream_trim_ms = static_cast<int16_t>(parsed);
    } else if (key == "device_name") {
      copyString(loaded.device_name, sizeof(loaded.device_name), value.c_str());
      sanitizeDeviceNameInPlace(loaded.device_name, sizeof(loaded.device_name));
      device_name_loaded = true;
    } else if (key == "eq_preset") {
      long preset_val = value.toInt();
      if (preset_val >= 0 && preset_val <= static_cast<long>(EqPreset::kCustom)) {
        loaded.eq_preset = static_cast<EqPreset>(preset_val);
      }
    } else if (key == "eq_bass_gain_db") {
      loaded.eq_bass_gain_db = static_cast<int8_t>(value.toInt());
    } else if (key == "eq_treble_gain_db") {
      loaded.eq_treble_gain_db = static_cast<int8_t>(value.toInt());
    } else if (key == "eq_bass_freq_hz") {
      loaded.eq_bass_freq_hz = static_cast<uint16_t>(value.toInt());
    } else if (key == "eq_treble_freq_hz") {
      loaded.eq_treble_freq_hz = static_cast<uint16_t>(value.toInt());
    }
  }

  file.close();

  // Migrate legacy static default names to a unique MAC-based default.
  if (!device_name_loaded || loaded.device_name[0] == '\0' ||
      strcmp(loaded.device_name, kAppConfig.network.client_name) == 0) {
    buildDefaultDeviceName(loaded.device_name, sizeof(loaded.device_name));
    sanitizeDeviceNameInPlace(loaded.device_name, sizeof(loaded.device_name));
    saveNetworkSettings(loaded);
    loaded = network_settings_;
  }

  normalizeEqSettings(&loaded);

  network_settings_ = loaded;
  LOGI(kTag, "Network settings loaded from %s: ssid='%s' host='%s' port=%u",
       kNetworkConfigPath, network_settings_.wifi_ssid, network_settings_.snapcast_host,
       static_cast<unsigned>(network_settings_.snapcast_port));
  return true;
}

const VoiceSettings &StorageService::voiceSettings() const {
  return voice_settings_;
}

bool StorageService::saveVoiceSettings(const VoiceSettings &settings) {
  VoiceSettings normalized = settings;
  if (normalized.ha_port <= 0 || normalized.ha_port > 65535) {
    normalized.ha_port = 8123;
  }

  File file = SPIFFS.open(kVoiceConfigPath, FILE_WRITE, true);
  if (!file) {
    LOGE(kTag, "Failed to open %s for writing", kVoiceConfigPath);
    return false;
  }

  file.printf("ha_host=%s\n", normalized.ha_host);
  file.printf("ha_port=%d\n", static_cast<int>(normalized.ha_port));
  file.printf("ha_token=%s\n", normalized.ha_token);
  file.printf("ha_pipeline_id=%s\n", normalized.ha_pipeline_id);
  file.close();

  voice_settings_ = normalized;
  LOGI(kTag, "Voice settings saved to %s", kVoiceConfigPath);
  return true;
}

bool StorageService::resetVoiceParam(const char *param_name) {
  if (!param_name) {
    return false;
  }

  VoiceSettings vs = voice_settings_;
  if (strcmp(param_name, "ha_host") == 0) {
    vs.ha_host[0] = '\0';
  } else if (strcmp(param_name, "ha_port") == 0) {
    vs.ha_port = 8123;
  } else if (strcmp(param_name, "ha_token") == 0) {
    vs.ha_token[0] = '\0';
  } else if (strcmp(param_name, "ha_pipeline_id") == 0) {
    vs.ha_pipeline_id[0] = '\0';
  } else {
    return false;
  }
  return saveVoiceSettings(vs);
}

void StorageService::loadVoiceSettings() {
  voice_settings_.ha_host[0] = '\0';
  voice_settings_.ha_port = 8123;
  voice_settings_.ha_token[0] = '\0';
  voice_settings_.ha_pipeline_id[0] = '\0';

  File file = SPIFFS.open(kVoiceConfigPath, FILE_READ);
  if (!file) {
    LOGI(kTag, "No voice config found at %s, using defaults", kVoiceConfigPath);
    return;
  }

  while (file.available()) {
    String raw_line = file.readStringUntil('\n');
    raw_line.trim();
    if (raw_line.isEmpty() || raw_line.startsWith("#")) {
      continue;
    }
    const int sep = raw_line.indexOf('=');
    if (sep <= 0) {
      continue;
    }
    String key = raw_line.substring(0, sep);
    String value = raw_line.substring(sep + 1);
    key.trim();
    value.trim();

    if (key == "ha_host") {
      strncpy(voice_settings_.ha_host, value.c_str(), sizeof(voice_settings_.ha_host) - 1);
      voice_settings_.ha_host[sizeof(voice_settings_.ha_host) - 1] = '\0';
    } else if (key == "ha_port") {
      const long p = value.toInt();
      if (p >= 1 && p <= 65535) {
        voice_settings_.ha_port = static_cast<int32_t>(p);
      }
    } else if (key == "ha_token") {
      strncpy(voice_settings_.ha_token, value.c_str(), sizeof(voice_settings_.ha_token) - 1);
      voice_settings_.ha_token[sizeof(voice_settings_.ha_token) - 1] = '\0';
    } else if (key == "ha_pipeline_id") {
      strncpy(voice_settings_.ha_pipeline_id, value.c_str(), sizeof(voice_settings_.ha_pipeline_id) - 1);
      voice_settings_.ha_pipeline_id[sizeof(voice_settings_.ha_pipeline_id) - 1] = '\0';
    }
  }
  file.close();
  LOGI(kTag, "Voice settings loaded: host='%s' port=%d", voice_settings_.ha_host,
       static_cast<int>(voice_settings_.ha_port));
}

}  // namespace snapcast
