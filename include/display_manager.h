#pragma once

#include <Arduino.h>
#include <U8g2lib.h>

namespace snapcast {

class DisplayManager {
 public:
  DisplayManager();
  bool begin();
  void tick();

  void setWifiState(bool connected, const char *ip);
  void setSnapcastState(bool connected);
  void setVolumeState(uint32_t volume_percent, bool muted);
  void setNowPlaying(const char *title, const char *artist, const char *album, bool playing);

 private:
  struct ScrollState {
    int16_t offset_px = 0;
    bool forward = true;
    uint32_t last_step_ms = 0;
    uint32_t hold_until_ms = 0;
  };

  void drawScreen();
  void resetScroll(ScrollState &state);
  void drawScrollableTextLine(uint8_t y, const char *prefix, const String &text,
                              ScrollState &state, uint32_t now_ms);

  U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI display_;
  bool initialized_ = false;
  bool wifi_connected_ = false;
  bool snapcast_connected_ = false;
  bool playing_ = false;
  bool muted_ = false;
  uint32_t volume_percent_ = 100;
  String ip_;
  String title_;
  String artist_;
  String album_;
  uint32_t boot_ms_ = 0;
  uint32_t last_draw_ms_ = 0;
  uint32_t wifi_connected_ms_ = 0;
  ScrollState title_scroll_;
  ScrollState artist_scroll_;
  ScrollState album_scroll_;
};

}  // namespace snapcast
