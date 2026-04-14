#pragma once

#include <stdint.h>

#ifndef APP_SYNC_TRIM_MS
#define APP_SYNC_TRIM_MS 0
#endif

namespace snapcast {

struct AudioPins {
  int8_t port;
  int8_t bclk;
  int8_t ws;
  int8_t data_out;
  int8_t data_in;
};

struct AudioConfig {
  uint32_t sample_rate_hz;
  uint16_t tone_frequency_hz;
  uint16_t tone_duration_ms;
  int16_t tone_amplitude;
  int16_t sync_trim_ms;
  bool play_startup_tone;
  bool swap_pcm_frame_words;
  uint8_t pcm_queue_depth;
  uint16_t pcm_chunk_max_size;
};

struct NetworkConfig {
  const char *wifi_ssid;
  const char *wifi_password;
  const char *snapcast_host;
  uint16_t snapcast_port;
  uint32_t wifi_retry_interval_ms;
  uint32_t server_retry_interval_ms;
  const char *client_name;
  const char *client_version;
  const char *client_os;
  const char *client_arch;
  uint8_t protocol_version;
};

struct DisplayPins {
  int8_t cs;
  int8_t dc;
  int8_t reset;
  int8_t sck;
  int8_t mosi;
};

struct AppConfig {
  const char *device_name;
  uint32_t serial_baudrate;
  uint32_t heartbeat_period_ms;
  uint32_t main_loop_delay_ms;
  int8_t wifi_reset_button_gpio;
  uint32_t wifi_reset_hold_ms;
  int8_t status_led_gpio;
  AudioConfig audio_runtime;
  AudioPins audio;
  NetworkConfig network;
  DisplayPins display;
};

inline constexpr AppConfig kAppConfig = {
    "Snapcast_v4_arduino",
    115200,
    5000,
    1,
    0,
    2500,
    14,
    {
        44100,
        440,
        1200,
        4000,
        APP_SYNC_TRIM_MS,
        false,
        false,
        16,    // 16 chunks — sync buffer 280ms (14 chunks needed) + 2 headroom; TTS: 16×~2KB=32KB safe
      4096,
    },
    {
        0,
        26,
        25,
        22,
        13,
    },
    {
      "",
      "",
      "192.168.0.21",
      1704,
      10000,
      5000,
      "Snapcast_v4_arduino",
      "0.0.1",
      "ESP32-Arduino",
      "xtensa",
      2,
    },
    {
        15,
        4,
        32,
        18,
        23,
    },
};

}  // namespace snapcast
