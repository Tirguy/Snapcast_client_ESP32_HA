#include "display_manager.h"

#include <SPI.h>

#include "app_config.h"
#include "logging.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "display";
constexpr uint32_t kDrawPeriodMs = 200;
constexpr uint32_t kBootStatusPeriodMs = 3000;
constexpr uint32_t kIpDisplayAfterWifiMs = 5000;
constexpr uint32_t kScrollStepMs = 120;
constexpr uint32_t kScrollEdgeHoldMs = 800;
}

DisplayManager::DisplayManager()
    : display_(U8G2_R0, kAppConfig.display.cs, kAppConfig.display.dc, kAppConfig.display.reset) {}

bool DisplayManager::begin() {
  SPI.begin(kAppConfig.display.sck, -1, kAppConfig.display.mosi, kAppConfig.display.cs);
  display_.begin();
  display_.setFont(u8g2_font_6x12_tf);
  boot_ms_ = millis();
  initialized_ = true;
  LOGI(kTag, "OLED initialized");
  return true;
}

void DisplayManager::setWifiState(bool connected, const char *ip) {
  const bool was_connected = wifi_connected_;
  wifi_connected_ = connected;
  ip_ = ip ? String(ip) : String();
  if (wifi_connected_ && !was_connected) {
    wifi_connected_ms_ = millis();
  }
  if (!wifi_connected_) {
    wifi_connected_ms_ = 0;
  }
}

void DisplayManager::setSnapcastState(bool connected) { snapcast_connected_ = connected; }

void DisplayManager::setVolumeState(uint32_t volume_percent, bool muted) {
  volume_percent_ = (volume_percent > 100) ? 100 : volume_percent;
  muted_ = muted;
}

void DisplayManager::setNowPlaying(const char *title, const char *artist, const char *album,
                                   bool playing) {
  const String new_title = title ? String(title) : String();
  const String new_artist = artist ? String(artist) : String();
  const String new_album = album ? String(album) : String();

  if (new_title != title_) {
    resetScroll(title_scroll_);
  }
  if (new_artist != artist_) {
    resetScroll(artist_scroll_);
  }
  if (new_album != album_) {
    resetScroll(album_scroll_);
  }

  title_ = new_title;
  artist_ = new_artist;
  album_ = new_album;
  playing_ = playing && (!title_.isEmpty() || !artist_.isEmpty() || !album_.isEmpty());
}

void DisplayManager::tick() {
  if (!initialized_) {
    return;
  }

  const uint32_t now = millis();
  if (now - last_draw_ms_ < kDrawPeriodMs) {
    return;
  }
  last_draw_ms_ = now;
  drawScreen();
}

void DisplayManager::resetScroll(ScrollState &state) {
  state.offset_px = 0;
  state.forward = true;
  state.last_step_ms = 0;
  state.hold_until_ms = 0;
}

void DisplayManager::drawScrollableTextLine(uint8_t y, const char *prefix, const String &text,
                                            ScrollState &state, uint32_t now_ms) {
  display_.setCursor(0, y);
  display_.print(prefix);

  const int16_t text_x = static_cast<int16_t>(display_.getStrWidth(prefix));
  const int16_t max_width = 128 - text_x;
  const int16_t text_width = static_cast<int16_t>(display_.getUTF8Width(text.c_str()));

  if (text_width <= max_width) {
    display_.setCursor(text_x, y);
    display_.print(text);
    resetScroll(state);
    return;
  }

  const int16_t max_offset = text_width - max_width;
  if (now_ms >= state.hold_until_ms &&
      (state.last_step_ms == 0 || now_ms - state.last_step_ms >= kScrollStepMs)) {
    state.last_step_ms = now_ms;
    if (state.forward) {
      if (state.offset_px < max_offset) {
        ++state.offset_px;
      } else {
        state.forward = false;
        state.hold_until_ms = now_ms + kScrollEdgeHoldMs;
      }
    } else {
      if (state.offset_px > 0) {
        --state.offset_px;
      } else {
        state.forward = true;
        state.hold_until_ms = now_ms + kScrollEdgeHoldMs;
      }
    }
  }

  display_.setClipWindow(text_x, y - 11, 128, y + 1);
  display_.setCursor(text_x - state.offset_px, y);
  display_.print(text);
  display_.setMaxClipWindow();
}

void DisplayManager::drawScreen() {
  const uint32_t now_ms = millis();
  const bool show_metadata = (now_ms - boot_ms_ >= kBootStatusPeriodMs) && playing_;
  const bool show_ip = wifi_connected_ && wifi_connected_ms_ > 0 &&
                       (now_ms - wifi_connected_ms_ < kIpDisplayAfterWifiMs);

  display_.clearBuffer();
  display_.setCursor(0, 12);
  if (show_ip) {
    display_.print("IP: ");
    display_.print(ip_);
  } else {
    display_.print("VOL: ");
    if (muted_) {
      display_.print("MUTED");
    } else {
      display_.print(volume_percent_);
      display_.print('%');
    }
  }

  display_.setCursor(0, 24);
  display_.print("Snapcast: ");
  display_.print(snapcast_connected_ ? "OK" : "KO");

  if (show_metadata) {
    drawScrollableTextLine(38, "T: ", title_, title_scroll_, now_ms);
    drawScrollableTextLine(50, "A: ", artist_, artist_scroll_, now_ms);
    drawScrollableTextLine(62, "Al:", album_, album_scroll_, now_ms);
  }

  display_.sendBuffer();
}

}  // namespace snapcast
