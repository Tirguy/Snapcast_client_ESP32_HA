#include "metadata_manager.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "metadata";
constexpr uint32_t kPollIntervalMs = 5000;
}

bool MetadataManager::begin() {
  // Keep this short to avoid stalling the main loop and starving audio ingest.
  client_.setTimeout(120);
  return true;
}

void MetadataManager::setTarget(const char *host, uint16_t stream_port) {
  if (host) {
    host_ = host;
  }
  stream_port_ = stream_port;
  control_port_ = static_cast<uint16_t>(stream_port_ + 1);
}

void MetadataManager::tick() {
  if (WiFi.status() != WL_CONNECTED || host_.isEmpty()) {
    playing_ = false;
    return;
  }

  const uint32_t now = millis();
  if (now - last_poll_ms_ < kPollIntervalMs) {
    return;
  }
  last_poll_ms_ = now;
  pollServerStatus();
}

bool MetadataManager::pollServerStatus() {
  if (!client_.connect(host_.c_str(), control_port_)) {
    return false;
  }

  static const char *request =
      "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetStatus\"}\r\n";
  if (client_.write(reinterpret_cast<const uint8_t *>(request), strlen(request)) == 0) {
    client_.stop();
    return false;
  }

  String line;
  line.reserve(4096);
  const uint32_t deadline = millis() + 120;
  bool parsed = false;
  while (millis() < deadline) {
    while (client_.available() > 0) {
      const char c = static_cast<char>(client_.read());
      if (c == '\n' || c == '\r') {
        if (!line.isEmpty()) {
          parsed = parseServerStatusFrame(line) || parsed;
          line = "";
        }
      } else {
        line += c;
      }
    }
    if (!client_.connected() && client_.available() == 0) {
      break;
    }
    delay(1);
  }

  if (!line.isEmpty()) {
    parsed = parseServerStatusFrame(line) || parsed;
  }

  client_.stop();
  return parsed;
}

bool MetadataManager::parseServerStatusFrame(const String &json_line) {
  DynamicJsonDocument doc(12288);
  const auto err = deserializeJson(doc, json_line);
  if (err) {
    return false;
  }

  JsonVariant result = doc["result"];
  if (result.isNull()) {
    return false;
  }

  JsonArray streams = result["server"]["streams"].as<JsonArray>();
  if (streams.isNull()) {
    return false;
  }

  bool found = false;
  bool playing = false;
  String title;
  String artist;
  String album;

  for (JsonObject stream : streams) {
    JsonObject properties = stream["properties"].as<JsonObject>();
    if (properties.isNull()) {
      continue;
    }

    const char *playback_status = properties["playbackStatus"] | "";
    const bool is_playing = strcmp(playback_status, "playing") == 0;
    JsonObject metadata = properties["metadata"].as<JsonObject>();
    if (metadata.isNull()) {
      continue;
    }

    String t = metadata["title"] | "";
    String ar;
    if (metadata["artist"].is<JsonArray>()) {
      JsonArray artists = metadata["artist"].as<JsonArray>();
      if (!artists.isNull() && artists.size() > 0) {
        ar = artists[0].as<const char *>();
      }
    } else {
      ar = metadata["artist"] | "";
    }
    String al = metadata["album"] | "";

    if (is_playing) {
      title = t;
      artist = ar;
      album = al;
      found = true;
      playing = true;
      break;
    }

    if (!found) {
      title = t;
      artist = ar;
      album = al;
      found = true;
    }
  }

  if (found) {
    title_ = title;
    artist_ = artist;
    album_ = album;
    playing_ = playing && (!title_.isEmpty() || !artist_.isEmpty() || !album_.isEmpty());
  } else {
    playing_ = false;
  }

  return found;
}

bool MetadataManager::isPlaying() const { return playing_; }

const String &MetadataManager::title() const { return title_; }

const String &MetadataManager::artist() const { return artist_; }

const String &MetadataManager::album() const { return album_; }

}  // namespace snapcast
