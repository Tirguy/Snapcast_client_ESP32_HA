#include "network_manager.h"

#include <esp_system.h>

#include <WiFi.h>

#include "app_config.h"
#include "application.h"
#include "logging.h"
#include "snapcast_client.h"

namespace snapcast {
namespace {
constexpr const char *kTag = "network";
constexpr uint32_t kPortalStartDelayMs = 15000;
constexpr uint32_t kPortalRetryMs = 3000;
constexpr uint32_t kPortalRedirectGraceMs = 12000;
constexpr uint32_t kResetButtonReleaseGuardMs = 2000;
constexpr uint32_t kRebootDelayAfterResetMs = 250;
constexpr byte kDnsPort = 53;
constexpr const char *kPortalIp = "192.168.4.1";
SnapcastClient g_snapcast_client;

void onServerSettingsFromSnapcast(uint32_t volume_percent, bool muted,
                                  int32_t latency_ms, int32_t buffer_ms) {
  Application::instance().onServerSettings(volume_percent, muted, latency_ms,
                                           buffer_ms);
}

void onCodecHeaderPcmFromSnapcast(uint32_t sample_rate_hz, uint16_t bits_per_sample,
                                  uint16_t channels) {
  Application::instance().onCodecHeaderPcm(sample_rate_hz, bits_per_sample, channels);
}

void onWireChunkPcmFromSnapcast(const uint8_t *payload, size_t size,
                               int64_t chunk_ts_us) {
  Application::instance().onWireChunkPcm(payload, size, chunk_ts_us);
}

void onSyncOffsetFromSnapcast(int64_t offset_us) {
  Application::instance().onSyncOffsetUpdated(offset_us);
}
}

bool NetworkManager::begin() {
  g_snapcast_client.setServerSettingsCallback(onServerSettingsFromSnapcast);
  g_snapcast_client.setCodecHeaderPcmCallback(onCodecHeaderPcmFromSnapcast);
  g_snapcast_client.setWireChunkPcmCallback(onWireChunkPcmFromSnapcast);
  g_snapcast_client.setSyncOffsetCallback(onSyncOffsetFromSnapcast);

  portal_server_.on("/", HTTP_GET, [this]() { handlePortalRoot(); });
  portal_server_.on("/save", HTTP_POST, [this]() { handlePortalSave(); });
  portal_server_.on("/status", HTTP_GET, [this]() { handlePortalStatus(); });
    portal_server_.on("/get", HTTP_GET, [this]() { handleGetParam(); });
    portal_server_.on("/post", HTTP_POST, [this]() { handlePostParam(); });
    portal_server_.on("/delete", HTTP_DELETE, [this]() { handleDeleteParam(); });
    portal_server_.on("/scan", HTTP_GET, [this]() { handlePortalScan(); });
  portal_server_.onNotFound([this]() {
    if (portal_running_) {
      portal_server_.sendHeader("Location", String("http://") + kPortalIp + "/", true);
      portal_server_.send(302, "text/plain", "Redirecting to portal");
      return;
    }
    portal_server_.sendHeader("Location", "/", true);
    portal_server_.send(302, "text/plain", "Redirecting to config");
  });

  if (kAppConfig.wifi_reset_button_gpio >= 0) {
    pinMode(kAppConfig.wifi_reset_button_gpio, INPUT_PULLUP);
    reset_button_guard_started_ms_ = millis();
    reset_button_initialized_ = true;
    LOGI(kTag, "WiFi reset button enabled on GPIO%d (hold %lu ms)",
         kAppConfig.wifi_reset_button_gpio,
         static_cast<unsigned long>(kAppConfig.wifi_reset_hold_ms));
  }

  return configureWifi();
}

void NetworkManager::tick() {
  maintainResetButton();
  maintainWifi();
  maintainProvisioningPortal();
  maintainSnapcastClient();
  g_snapcast_client.tick();
}

bool NetworkManager::configureWifi() {
  const RuntimeNetworkSettings &settings = Application::instance().storage().networkSettings();
  g_snapcast_client.setClientName(settings.device_name);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (strlen(settings.wifi_ssid) == 0) {
    LOGI(kTag, "WiFi not configured yet; provisioning portal requested");
    portal_start_requested_ = true;
    return true;
  }

  return beginStationConnection();
}

bool NetworkManager::beginStationConnection() {
  const RuntimeNetworkSettings &settings = Application::instance().storage().networkSettings();
  if (strlen(settings.wifi_ssid) == 0) {
    return false;
  }

  WiFi.mode(portal_running_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(settings.wifi_ssid, settings.wifi_password);
  last_wifi_attempt_ms_ = millis();
  wifi_connect_started_ms_ = last_wifi_attempt_ms_;
  LOGI(kTag, "WiFi connection started for SSID '%s'%s", settings.wifi_ssid,
       settings.loaded_from_file ? " (SPIFFS)" : " (defaults)");
  return true;
}

void NetworkManager::maintainWifi() {
  const RuntimeNetworkSettings &settings = Application::instance().storage().networkSettings();
  static bool last_wifi_connected = false;
  static String last_ip;
  static uint32_t last_portal_retry_ms = 0;

  if (WiFi.status() != WL_CONNECTED && last_wifi_connected) {
    Application::instance().onWifiStateChanged(false, nullptr);
    last_wifi_connected = false;
    last_ip = "";
  }

  if (strlen(settings.wifi_ssid) == 0) {
    const uint32_t now_ms = millis();
    if ((portal_start_requested_ || !portal_running_) &&
        (now_ms - last_portal_retry_ms >= kPortalRetryMs)) {
      last_portal_retry_ms = now_ms;
      LOGI(kTag, "No WiFi SSID configured, ensuring provisioning AP is running");
      startProvisioningPortal();
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    static bool logged = false;
    const String ip = WiFi.localIP().toString();
    if (!logged) {
      LOGI(kTag, "WiFi connected, IP=%s", ip.c_str());
      logged = true;
    }
    if (!last_wifi_connected || last_ip != ip) {
      Application::instance().onWifiStateChanged(true, ip.c_str());
      last_wifi_connected = true;
      last_ip = ip;
    }
    if (!config_server_started_) {
      portal_server_.begin();
      config_server_started_ = true;
      LOGI(kTag, "Configuration web UI started on STA (%s)", ip.c_str());
    }

    if (portal_running_) {
      const bool redirect_window_open =
          portal_redirect_pending_ &&
          (millis() - portal_redirect_started_ms_ < kPortalRedirectGraceMs);
      if (!redirect_window_open) {
        stopProvisioningPortal();
      }
    }
    return;
  }

  static bool logged = false;
  logged = false;

  if (!portal_running_ && wifi_connect_started_ms_ != 0 &&
      (millis() - wifi_connect_started_ms_) >= kPortalStartDelayMs) {
    LOGW(kTag, "WiFi connection timeout, starting provisioning portal");
    startProvisioningPortal();
  }

  if (millis() - last_wifi_attempt_ms_ < kAppConfig.network.wifi_retry_interval_ms) {
    return;
  }

  last_wifi_attempt_ms_ = millis();
  LOGI(kTag, "Retrying WiFi connection");
  WiFi.disconnect();
  beginStationConnection();
}

void NetworkManager::maintainSnapcastClient() {
  const RuntimeNetworkSettings &settings = Application::instance().storage().networkSettings();
  g_snapcast_client.setClientName(settings.device_name);
  static bool last_snapcast_connected = false;

  if (WiFi.status() != WL_CONNECTED) {
    if (g_snapcast_client.isConnected()) {
      g_snapcast_client.disconnect();
    }
    if (last_snapcast_connected) {
      Application::instance().onSnapcastStateChanged(false);
      last_snapcast_connected = false;
    }
    return;
  }

  if (strlen(settings.snapcast_host) == 0) {
    return;
  }

  if (g_snapcast_client.isConnected()) {
    if (!last_snapcast_connected) {
      Application::instance().onSnapcastStateChanged(true);
      last_snapcast_connected = true;
    }
    return;
  }

  if (millis() - last_server_attempt_ms_ < kAppConfig.network.server_retry_interval_ms) {
    return;
  }

  last_server_attempt_ms_ = millis();
  const bool connected = g_snapcast_client.connect(settings.snapcast_host, settings.snapcast_port);
  if (!connected) {
    LOGE(kTag, "Snapcast TCP connection failed: %s:%u", settings.snapcast_host,
         settings.snapcast_port);
    if (last_snapcast_connected) {
      Application::instance().onSnapcastStateChanged(false);
      last_snapcast_connected = false;
    }
  } else if (!last_snapcast_connected) {
    Application::instance().onSnapcastStateChanged(true);
    last_snapcast_connected = true;
  }
}

void NetworkManager::maintainProvisioningPortal() {
  if (portal_running_) {
    dns_server_.processNextRequest();
  }
  if (config_server_started_) {
    portal_server_.handleClient();
  }
}

void NetworkManager::maintainResetButton() {
  if (!reset_button_initialized_ || kAppConfig.wifi_reset_button_gpio < 0) {
    return;
  }

  if (reboot_pending_after_reset_) {
    const bool still_pressed = (digitalRead(kAppConfig.wifi_reset_button_gpio) == LOW);
    if (still_pressed) {
      return;
    }

    const uint32_t now_ms = millis();
    if (now_ms - reboot_request_ms_ < kRebootDelayAfterResetMs) {
      return;
    }

    LOGW(kTag, "Rebooting after WiFi reset to restart in provisioning mode");
    esp_restart();
    return;
  }

  const uint32_t now_ms = millis();
  if (now_ms - reset_button_guard_started_ms_ < kResetButtonReleaseGuardMs) {
    return;
  }

  const bool pressed = (digitalRead(kAppConfig.wifi_reset_button_gpio) == LOW);
  if (pressed) {
    if (reset_button_pressed_since_ms_ == 0) {
      reset_button_pressed_since_ms_ = now_ms;
    }

    if (!reset_button_action_latched_ &&
        (now_ms - reset_button_pressed_since_ms_) >= kAppConfig.wifi_reset_hold_ms) {
      reset_button_action_latched_ = true;
      LOGW(kTag,
           "GPIO%d long press detected -> reset WiFi credentials and start provisioning",
           kAppConfig.wifi_reset_button_gpio);
      if (!resetWifiCredentialsAndStartProvisioning()) {
        LOGE(kTag, "Failed to reset WiFi credentials from button");
      }
    }
  } else {
    reset_button_pressed_since_ms_ = 0;
    reset_button_action_latched_ = false;
  }
}

bool NetworkManager::resetWifiCredentialsAndStartProvisioning() {
  if (!Application::instance().storage().resetWifiSettings()) {
    return false;
  }

  if (g_snapcast_client.isConnected()) {
    g_snapcast_client.disconnect();
  }

  Application::instance().onSnapcastStateChanged(false);
  Application::instance().onWifiStateChanged(false, nullptr);

  WiFi.disconnect(true, false);
  wifi_connect_started_ms_ = 0;
  last_wifi_attempt_ms_ = 0;
  last_server_attempt_ms_ = 0;

  // Request a clean reboot (after button release) so boot path immediately enters AP provisioning.
  portal_start_requested_ = true;
  reboot_pending_after_reset_ = true;
  reboot_request_ms_ = millis();
  LOGW(kTag, "WiFi credentials cleared, reboot pending after button release");
  return true;
}

void NetworkManager::startProvisioningPortal() {
  if (portal_running_) {
    portal_start_requested_ = false;
    return;
  }

  WiFi.disconnect();
  // Keep provisioning in AP-only mode for maximum stability.
  WiFi.mode(WIFI_AP);
  delay(20);

  IPAddress ap_ip;
  IPAddress gateway;
  IPAddress subnet;
  ap_ip.fromString(kPortalIp);
  gateway.fromString(kPortalIp);
  subnet.fromString("255.255.255.0");
  WiFi.softAPConfig(ap_ip, gateway, subnet);

  const String ssid = provisioningSsid();
  if (!WiFi.softAP(ssid.c_str())) {
    LOGE(kTag, "Failed to start provisioning AP");
    return;
  }

  dns_server_.start(kDnsPort, "*", ap_ip);
  if (!config_server_started_) {
    portal_server_.begin();
    config_server_started_ = true;
    LOGI(kTag, "Configuration web UI started on AP (%s)", kPortalIp);
  }
  portal_running_ = true;
  portal_start_requested_ = false;
  LOGI(kTag, "Provisioning AP started: SSID='%s' IP=%s", ssid.c_str(), kPortalIp);
}

void NetworkManager::stopProvisioningPortal() {
  if (!portal_running_) {
    return;
  }

  dns_server_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portal_running_ = false;
  portal_start_requested_ = false;
  portal_redirect_pending_ = false;
  portal_redirect_started_ms_ = 0;
  LOGI(kTag, "Provisioning portal stopped");
}

void NetworkManager::handlePortalRoot() {
  portal_server_.send(200, "text/html; charset=utf-8", buildPortalHtml(portal_running_));
}

void NetworkManager::handlePortalSave() {
  RuntimeNetworkSettings updated = Application::instance().storage().networkSettings();

  const String ssid = portal_server_.arg("wifi_ssid");
  const String password = portal_server_.arg("wifi_password");
  const String host = portal_server_.arg("snapcast_host");
  const String port_text = portal_server_.arg("snapcast_port");
  const String stream_trim_text = portal_server_.arg("stream_trim_ms");
  const String device_name = portal_server_.arg("device_name");
  const String eq_preset_text = portal_server_.arg("eq_preset");
  const String eq_bass_gain_text = portal_server_.arg("eq_bass_gain_db");
  const String eq_treble_gain_text = portal_server_.arg("eq_treble_gain_db");
  const String eq_bass_freq_text = portal_server_.arg("eq_bass_freq_hz");
  const String eq_treble_freq_text = portal_server_.arg("eq_treble_freq_hz");
  const bool device_name_changed =
      (device_name.length() > 0) && (device_name != String(updated.device_name));

  if (portal_running_) {
    if (ssid.isEmpty() || host.isEmpty()) {
      portal_server_.send(400, "text/plain", "wifi_ssid and snapcast_host are required");
      return;
    }

    strncpy(updated.wifi_ssid, ssid.c_str(), sizeof(updated.wifi_ssid) - 1);
    updated.wifi_ssid[sizeof(updated.wifi_ssid) - 1] = '\0';
    strncpy(updated.wifi_password, password.c_str(), sizeof(updated.wifi_password) - 1);
    updated.wifi_password[sizeof(updated.wifi_password) - 1] = '\0';
    strncpy(updated.snapcast_host, host.c_str(), sizeof(updated.snapcast_host) - 1);
    updated.snapcast_host[sizeof(updated.snapcast_host) - 1] = '\0';
  } else {
    // In STA mode, Wi-Fi credentials are not editable from this page.
    if (!host.isEmpty()) {
      strncpy(updated.snapcast_host, host.c_str(), sizeof(updated.snapcast_host) - 1);
      updated.snapcast_host[sizeof(updated.snapcast_host) - 1] = '\0';
    }
  }

  long parsed_port = port_text.toInt();
  if (parsed_port <= 0 || parsed_port > 65535) {
    parsed_port = kAppConfig.network.snapcast_port;
  }
  updated.snapcast_port = static_cast<uint16_t>(parsed_port);
  long parsed_trim = stream_trim_text.toInt();
  if (parsed_trim < -2000) {
    parsed_trim = -2000;
  }
  if (parsed_trim > 2000) {
    parsed_trim = 2000;
  }
  updated.stream_trim_ms = static_cast<int16_t>(parsed_trim);
  if (device_name.length() > 0) {
    strncpy(updated.device_name, device_name.c_str(), sizeof(updated.device_name) - 1);
    updated.device_name[sizeof(updated.device_name) - 1] = '\0';
  }
  if (eq_preset_text.length() > 0) {
    long preset_val = eq_preset_text.toInt();
    if (preset_val >= 0 && preset_val <= 4) {
      updated.eq_preset = static_cast<EqPreset>(preset_val);
    }
  }
  if (eq_bass_gain_text.length() > 0) {
    long parsed = eq_bass_gain_text.toInt();
    if (parsed < -12) {
      parsed = -12;
    }
    if (parsed > 12) {
      parsed = 12;
    }
    updated.eq_bass_gain_db = static_cast<int8_t>(parsed);
  }
  if (eq_treble_gain_text.length() > 0) {
    long parsed = eq_treble_gain_text.toInt();
    if (parsed < -12) {
      parsed = -12;
    }
    if (parsed > 12) {
      parsed = 12;
    }
    updated.eq_treble_gain_db = static_cast<int8_t>(parsed);
  }
  if (eq_bass_freq_text.length() > 0) {
    long parsed = eq_bass_freq_text.toInt();
    if (parsed < 60) {
      parsed = 60;
    }
    if (parsed > 400) {
      parsed = 400;
    }
    updated.eq_bass_freq_hz = static_cast<uint16_t>(parsed);
  }
  if (eq_treble_freq_text.length() > 0) {
    long parsed = eq_treble_freq_text.toInt();
    if (parsed < 2000) {
      parsed = 2000;
    }
    if (parsed > 12000) {
      parsed = 12000;
    }
    updated.eq_treble_freq_hz = static_cast<uint16_t>(parsed);
  }

  if (!Application::instance().storage().saveNetworkSettings(updated)) {
    portal_server_.send(500, "text/plain", "Failed to save configuration");
    return;
  }

  Application::instance().onStreamTrimChanged(updated.stream_trim_ms);
  const RuntimeNetworkSettings &saved = Application::instance().storage().networkSettings();
  Application::instance().onEqSettingsChanged(saved);
  g_snapcast_client.setClientName(saved.device_name);
  if (device_name_changed && g_snapcast_client.isConnected()) {
    LOGI(kTag, "Device name changed, reconnecting Snapcast client");
    g_snapcast_client.disconnect();
    Application::instance().onSnapcastStateChanged(false);
  }
  if (portal_running_) {
    portal_redirect_pending_ = true;
    portal_redirect_started_ms_ = millis();

    String html;
    html.reserve(1200);
    html += "<!doctype html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Reconnecting</title></head><body style='font-family:Verdana,sans-serif;padding:24px'>";
    html += "<h1>Configuration saved</h1>";
    html += "<p>Connecting to WiFi... this page will redirect automatically to the speaker IP.</p>";
    html += "<p id='status'>Waiting for DHCP...</p>";
    html += "<script>";
    html += "let tries=0;";
    html += "async function poll(){";
    html += "tries++;";
    html += "try{";
    html += "const r=await fetch('/status',{cache:'no-store'});";
    html += "const s=await r.json();";
    html += "if(s.wifi_connected&&s.wifi_ip){document.getElementById('status').textContent='Redirecting to '+s.wifi_ip;window.location.href='http://'+s.wifi_ip+'/';return;}";
    html += "document.getElementById('status').textContent='Still connecting...';";
    html += "}catch(e){document.getElementById('status').textContent='Portal reconnecting...';}";
    html += "if(tries<40){setTimeout(poll,1000);}else{document.getElementById('status').textContent='Open your router DHCP list to find the speaker IP.';}";
    html += "}";
    html += "setTimeout(poll,800);";
    html += "</script></body></html>";
    portal_server_.send(200, "text/html; charset=utf-8", html);
    LOGI(kTag, "Provisioning data saved, reconnecting to WiFi");
    beginStationConnection();
    return;
  }

  portal_server_.send(200, "text/html; charset=utf-8",
                      "<!doctype html><html><body style='font-family:Verdana,sans-serif;padding:24px'><h1>Configuration saved</h1><p>Changes applied.</p><p><a href='/'>Back to configuration</a></p></body></html>");
  LOGI(kTag, "STA configuration saved");
}

void NetworkManager::handlePortalStatus() {
  portal_server_.send(200, "application/json", buildStatusJson());
}

void NetworkManager::handlePortalScan() {
  portal_server_.send(200, "application/json", buildScanJson());
}

void NetworkManager::handleGetParam() {
  const String param = portal_server_.arg("param");
  const VoiceSettings &vs = Application::instance().storage().voiceSettings();
  if (param == "ha_host") {
    portal_server_.send(200, "text/plain", vs.ha_host);
  } else if (param == "ha_port") {
    portal_server_.send(200, "text/plain", String(vs.ha_port));
  } else if (param == "ha_token") {
    portal_server_.send(200, "text/plain", vs.ha_token);
  } else if (param == "ha_pipeline_id") {
    portal_server_.send(200, "text/plain", vs.ha_pipeline_id);
  } else {
    portal_server_.send(404, "text/plain", "Unknown param");
  }
}

void NetworkManager::handlePostParam() {
  const String param = portal_server_.arg("param");
  const String value = portal_server_.arg("value");
  VoiceSettings vs = Application::instance().storage().voiceSettings();

  if (param == "ha_host") {
    strncpy(vs.ha_host, value.c_str(), sizeof(vs.ha_host) - 1);
    vs.ha_host[sizeof(vs.ha_host) - 1] = '\0';
  } else if (param == "ha_port") {
    const long p = value.toInt();
    if (p < 1 || p > 65535) {
      portal_server_.send(400, "text/plain", "Invalid port");
      return;
    }
    vs.ha_port = static_cast<int32_t>(p);
  } else if (param == "ha_token") {
    strncpy(vs.ha_token, value.c_str(), sizeof(vs.ha_token) - 1);
    vs.ha_token[sizeof(vs.ha_token) - 1] = '\0';
  } else if (param == "ha_pipeline_id") {
    strncpy(vs.ha_pipeline_id, value.c_str(), sizeof(vs.ha_pipeline_id) - 1);
    vs.ha_pipeline_id[sizeof(vs.ha_pipeline_id) - 1] = '\0';
  } else {
    portal_server_.send(404, "text/plain", "Unknown param");
    return;
  }

  if (!Application::instance().storage().saveVoiceSettings(vs)) {
    portal_server_.send(500, "text/plain", "Save failed");
    return;
  }
  portal_server_.send(200, "text/plain", "OK");
}

void NetworkManager::handleDeleteParam() {
  const String param = portal_server_.arg("param");
  if (!Application::instance().storage().resetVoiceParam(param.c_str())) {
    portal_server_.send(500, "text/plain", "Reset failed");
    return;
  }
  portal_server_.send(200, "text/plain", "OK");
}

String NetworkManager::buildPortalHtml(bool provisioning_mode) const {
  const RuntimeNetworkSettings &settings = Application::instance().storage().networkSettings();
  const wl_status_t wifi_status = WiFi.status();
  String html;
  html.reserve(20480);
  const VoiceSettings &vs = Application::instance().storage().voiceSettings();
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Snapcast Setup</title><style>";
  html += ":root{--bg:#f4efe6;--panel:#fffaf2;--ink:#1e2430;--muted:#5f6b7a;--accent:#0f766e;--line:#d8cfc2;}";
  html += "body{margin:0;font-family:Verdana,sans-serif;background:linear-gradient(180deg,#efe4d1,#f8f4ed);color:var(--ink);}";
  html += ".wrap{max-width:760px;margin:0 auto;padding:24px;}";
  html += ".card{background:var(--panel);border:1px solid var(--line);border-radius:18px;padding:22px;box-shadow:0 10px 30px rgba(0,0,0,.08);}";
  html += "h1{margin:0 0 10px;font-size:28px;}p{color:var(--muted);}label{display:block;margin:16px 0 6px;font-weight:bold;}";
  html += "input,select{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);box-sizing:border-box;background:#fff;}";
  html += ".row{display:flex;gap:10px;align-items:center;}";
  html += ".btn-inline{margin-top:0;padding:10px 12px;white-space:nowrap;}";
  html += ".btn-save{margin-top:20px;background:var(--accent);color:#fff;border:none;border-radius:12px;padding:12px 18px;font-weight:bold;cursor:pointer;}";
  html += ".tabs{display:flex;gap:10px;margin-top:14px;padding:8px;background:#efe8dd;border:1px solid var(--line);border-radius:14px;}";
  html += ".tab-btn{background:#f8f3ea;color:#2a2f38;border:1px solid #c9bda9;padding:8px 14px;border-radius:10px;cursor:pointer;font-weight:700;letter-spacing:.2px;}";
  html += ".tab-btn.active{background:#153b50;color:#fff;border-color:#153b50;box-shadow:inset 0 -2px 0 rgba(255,255,255,.2);}";
  html += ".tab{display:none;} .tab.active{display:block;}";
  html += ".hint{font-size:13px;color:#6b7280;margin-top:8px;}";
  html += ".meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:18px 0 6px;}.pill{background:#f3eee5;border:1px solid var(--line);padding:10px 12px;border-radius:12px;}";
  html += "</style></head><body><div class='wrap'><div class='card'>";
  if (provisioning_mode) {
    html += "<h1>Configuration WiFi Snapcast</h1>";
    html += "<p>Configurez le WiFi et les options de l'enceinte pour rejoindre votre reseau.</p>";
  } else {
    html += "<h1>Configuration de l'enceinte Snapcast</h1>";
    html += "<p>Mode STA connecte : les identifiants WiFi ne sont pas modifiables ici.</p>";
  }
  html += "<div class='meta'>";
  html += "<div class='pill'><strong>Nom enceinte</strong><br>";
  html += String(settings.device_name);
  html += "</div>";
  html += "<div class='pill'><strong>IP portail AP</strong><br>192.168.4.1</div>";
  html += "<div class='pill'><strong>Etat WiFi</strong><br>";
  html += (wifi_status == WL_CONNECTED) ? WiFi.localIP().toString() : String("non connecte");
  html += "</div></div>";
  html += "<div class='tabs'>";
  html += "<button type='button' class='tab-btn active' onclick=\"switchTab('network')\" id='tab-network'>Reseau</button>";
  html += "<button type='button' class='tab-btn' onclick=\"switchTab('audio')\" id='tab-audio'>Audio</button>";
  html += "<button type='button' class='tab-btn' onclick=\"switchTab('advanced')\" id='tab-advanced'>Avance</button>";
  html += "<button type='button' class='tab-btn' onclick=\"switchTab('voice')\" id='tab-voice'>Voix HA</button>";
  html += "</div>";
  html += "<div id='form-section'>";
  html += "<form method='POST' action='/save'>";
  html += "<div class='tab active' id='pane-network'>";
  if (provisioning_mode) {
    html += "<label for='wifi_ssid'>Nom du reseau WiFi (SSID)</label>";
    html += "<div class='row'>";
    html += "<select id='wifi_ssid_list' onchange=\"document.getElementById('wifi_ssid').value=this.value\"><option value=''>Choisir un reseau detecte...</option></select>";
    html += "<button class='btn-inline' type='button' onclick='scanWifi()'>Scanner</button>";
    html += "</div>";
    html += "<input id='wifi_ssid' name='wifi_ssid' value='" + String(settings.wifi_ssid) + "' required>";
    html += "<label for='wifi_password'>Mot de passe WiFi</label>";
    html += "<div class='row'>";
    html += "<input id='wifi_password' name='wifi_password' type='password' value='" + String(settings.wifi_password) + "'>";
    html += "<button class='btn-inline' type='button' onclick='togglePassword()'>Afficher</button>";
    html += "</div>";
  } else {
    html += "<label>Reseau WiFi</label><input value='" + String(settings.wifi_ssid) + "' disabled>";
    html += "<div class='hint'>Les identifiants WiFi ne peuvent etre modifies qu en mode AP de provisionnement.</div>";
  }
  html += "<label for='snapcast_host'>Adresse du serveur Snapcast</label>";
  html += "<input id='snapcast_host' name='snapcast_host' value='" + String(settings.snapcast_host) + "' required>";
  html += "<label for='snapcast_port'>Port du flux Snapcast</label>";
  html += "<input id='snapcast_port' name='snapcast_port' type='number' min='1' max='65535' value='" + String(settings.snapcast_port) + "' required>";
  html += "</div>";

  html += "<div class='tab' id='pane-audio'>";
  html += "<label for='stream_trim_ms'>Decalage du stream (ms)</label>";
  html += "<input id='stream_trim_ms' name='stream_trim_ms' type='number' min='-2000' max='2000' value='" + String(settings.stream_trim_ms) + "'>";
  html += "<div class='hint'>Valeur positive = retarde cette enceinte. A utiliser si cette enceinte est en avance.</div>";
  html += "<label for='eq_preset'>Preset egaliseur</label>";
  html += "<select id='eq_preset' name='eq_preset' onchange='applyEqPresetSelection()'>";
  html += String("<option value='0'") + (static_cast<uint8_t>(settings.eq_preset) == 0 ? " selected" : "") + ">Normal</option>";
  html += String("<option value='1'") + (static_cast<uint8_t>(settings.eq_preset) == 1 ? " selected" : "") + ">Bass Boost</option>";
  html += String("<option value='2'") + (static_cast<uint8_t>(settings.eq_preset) == 2 ? " selected" : "") + ">Treble Boost</option>";
  html += String("<option value='3'") + (static_cast<uint8_t>(settings.eq_preset) == 3 ? " selected" : "") + ">Bright</option>";
  html += String("<option value='4'") + (static_cast<uint8_t>(settings.eq_preset) == 4 ? " selected" : "") + ">Custom</option>";
  html += "</select>";
  html += "<div class='hint'>Choisissez un preset, ou passez en Custom pour ajuster finement.</div>";
  html += "<div id='eq-sliders'>";
  html += "<label for='eq_bass_gain_db'>Bass Gain (dB)</label>";
  html += "<input id='eq_bass_gain_db' name='eq_bass_gain_db' type='range' min='-12' max='12' step='1' value='" + String(settings.eq_bass_gain_db) + "' oninput='eqSliderChanged()'>";
  html += "<div class='hint'>Valeur actuelle: <strong id='eq_bass_gain_db_val'>" + String(settings.eq_bass_gain_db) + " dB</strong></div>";
  html += "<label for='eq_bass_freq_hz'>Bass Frequency (Hz)</label>";
  html += "<input id='eq_bass_freq_hz' name='eq_bass_freq_hz' type='range' min='60' max='400' step='10' value='" + String(settings.eq_bass_freq_hz) + "' oninput='eqSliderChanged()'>";
  html += "<div class='hint'>Valeur actuelle: <strong id='eq_bass_freq_hz_val'>" + String(settings.eq_bass_freq_hz) + " Hz</strong></div>";
  html += "<label for='eq_treble_gain_db'>Treble Gain (dB)</label>";
  html += "<input id='eq_treble_gain_db' name='eq_treble_gain_db' type='range' min='-12' max='12' step='1' value='" + String(settings.eq_treble_gain_db) + "' oninput='eqSliderChanged()'>";
  html += "<div class='hint'>Valeur actuelle: <strong id='eq_treble_gain_db_val'>" + String(settings.eq_treble_gain_db) + " dB</strong></div>";
  html += "<label for='eq_treble_freq_hz'>Treble Frequency (Hz)</label>";
  html += "<input id='eq_treble_freq_hz' name='eq_treble_freq_hz' type='range' min='2000' max='12000' step='100' value='" + String(settings.eq_treble_freq_hz) + "' oninput='eqSliderChanged()'>";
  html += "<div class='hint'>Valeur actuelle: <strong id='eq_treble_freq_hz_val'>" + String(settings.eq_treble_freq_hz) + " Hz</strong></div>";
  html += "</div>";
  html += "</div>";

  html += "<div class='tab' id='pane-advanced'>";
  html += "<label for='device_name'>Nom de l'enceinte</label>";
  html += "<input id='device_name' name='device_name' maxlength='32' pattern='[A-Za-z0-9 _-]+' value='" + String(settings.device_name) + "' required>";
  html += "<div class='hint'>Caracteres autorises: lettres, chiffres, espace, _ et -. Longueur max 32.</div>";
  html += "</div>";

  html += "<button type='submit' id='form-submit-btn' class='btn-save'>Enregistrer</button></form>";
  html += "</div>";
  html += "<div class='tab' id='pane-voice'>";
  html += "<div class='hint'>Connexion Home Assistant Voice Satellite (pipeline Assist). Configurez l hote, le token et le pipeline ID.</div>";
  html += "<label for='va-host'>Hote HA / IP</label>";
  html += "<input id='va-host' type='text' value='" + String(vs.ha_host) + "' placeholder='192.168.1.250 ou homeassistant.local' maxlength='127'>";
  html += "<label for='va-port'>Port HA</label>";
  html += "<input id='va-port' type='number' min='1' max='65535' value='" + String(vs.ha_port) + "' style='max-width:140px'>";
  html += "<div class='hint'>Port HTTP de Home Assistant (defaut 8123).</div>";
  html += "<div class='row' style='margin-top:14px'>";
  html += "<button type='button' class='btn-save' style='margin-top:0' onclick='vaSaveHostPort()'>Enregistrer hote</button>";
  html += "<button type='button' class='btn-save' style='margin-top:0;background:#6c757d' onclick='vaResetHostPort()'>Re-init</button>";
  html += "</div>";
  html += "<label for='va-token' style='margin-top:18px'>Token acces longue duree HA</label>";
  html += "<div class='hint'>Generer depuis HA : Profil &gt; Securite &gt; Jetons d acces longue duree.</div>";
  html += "<div class='row'>";
  html += "<input id='va-token' type='password' value='" + String(vs.ha_token) + "' placeholder='Collez le token ici' maxlength='511'>";
  html += "<button type='button' class='btn-inline' onclick='vaToggleToken()'>Voir</button>";
  html += "</div>";
  html += "<div class='row' style='margin-top:10px'>";
  html += "<button type='button' class='btn-save' style='margin-top:0' onclick='vaSaveToken()'>Enregistrer token</button>";
  html += "<button type='button' class='btn-save' style='margin-top:0;background:#6c757d' id='va-test-btn' onclick='vaTestToken()'>Tester token</button>";
  html += "<button type='button' class='btn-save' style='margin-top:0;background:#dc3545' onclick='vaClearToken()'>Effacer token</button>";
  html += "</div>";
  html += "<div id='va-token-status' style='margin-top:8px;padding:8px 12px;border-radius:6px;font-size:14px;border:1px solid #cfd8dc;display:none'></div>";
  html += "<label for='va-pipeline' style='margin-top:18px'>Pipeline ID Assist</label>";
  html += "<div class='hint'>Editable uniquement apres un test token valide. Copier depuis HA &gt; Parametres &gt; Assistants &gt; Pipelines.</div>";
  html += "<input id='va-pipeline' type='text' value='" + String(vs.ha_pipeline_id) + "' placeholder='ex: 01knqqpf69v30zn13xb1jjcjgw' maxlength='127' disabled>";
  html += "<div class='row' style='margin-top:10px'>";
  html += "<button type='button' class='btn-save' style='margin-top:0' id='va-save-pipeline-btn' onclick='vaSavePipeline()' disabled>Enregistrer pipeline</button>";
  html += "<button type='button' class='btn-save' style='margin-top:0;background:#6c757d' id='va-list-btn' onclick='vaListPipelines()' disabled>Lister pipelines HA</button>";
  html += "</div>";
  html += "<div id='va-pipeline-list'></div>";
  html += "</div>";
  html += "<script>";
  html += "function switchTab(name){['network','audio','advanced','voice'].forEach(t=>{var pe=document.getElementById('pane-'+t);var te=document.getElementById('tab-'+t);if(pe)pe.classList.toggle('active',t===name);if(te)te.classList.toggle('active',t===name);});var fs=document.getElementById('form-section');if(fs)fs.style.display=(name==='voice')?'none':'';}";
  html += "function togglePassword(){const p=document.getElementById('wifi_password');p.type=(p.type==='password')?'text':'password';}";
  html += "const EQ_PRESETS={0:{bg:0,bf:120,tg:0,tf:6000},1:{bg:6,bf:120,tg:0,tf:6000},2:{bg:0,bf:120,tg:6,tf:7000},3:{bg:-2,bf:140,tg:4,tf:8000}};";
  html += "function clampEqRanges(){const tf=document.getElementById('eq_treble_freq_hz');if(tf&&parseInt(tf.value,10)>12000){tf.value='12000';}}";
  html += "function updateEqVisibility(){const p=document.getElementById('eq_preset');const box=document.getElementById('eq-sliders');if(!p||!box)return;box.style.display=(p.value==='0')?'none':'block';}";
  html += "function updateEqLabels(){const bg=document.getElementById('eq_bass_gain_db');const bf=document.getElementById('eq_bass_freq_hz');const tg=document.getElementById('eq_treble_gain_db');const tf=document.getElementById('eq_treble_freq_hz');document.getElementById('eq_bass_gain_db_val').textContent=bg.value+' dB';document.getElementById('eq_bass_freq_hz_val').textContent=bf.value+' Hz';document.getElementById('eq_treble_gain_db_val').textContent=tg.value+' dB';document.getElementById('eq_treble_freq_hz_val').textContent=tf.value+' Hz';}";
  html += "function applyEqPresetSelection(){const p=document.getElementById('eq_preset').value;if(EQ_PRESETS[p]){document.getElementById('eq_bass_gain_db').value=EQ_PRESETS[p].bg;document.getElementById('eq_bass_freq_hz').value=EQ_PRESETS[p].bf;document.getElementById('eq_treble_gain_db').value=EQ_PRESETS[p].tg;document.getElementById('eq_treble_freq_hz').value=EQ_PRESETS[p].tf;}updateEqLabels();updateEqVisibility();}";
  html += "function eqSliderChanged(){clampEqRanges();document.getElementById('eq_preset').value='4';updateEqLabels();}";
  html += "async function scanWifi(){try{const r=await fetch('/scan');const d=await r.json();const sel=document.getElementById('wifi_ssid_list');if(!sel)return;sel.innerHTML='<option value=\"\">Choisir un reseau detecte...</option>';(d.networks||[]).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');sel.appendChild(o);});}catch(e){console.log(e);}}";
  html += "clampEqRanges();updateEqLabels();updateEqVisibility();";
  html += "var vaTokenOk=false;";
  html += "function vaToggleToken(){var i=document.getElementById('va-token');i.type=(i.type==='password')?'text':'password';}";
  html += "function vaSetPipelineLock(ok){var pi=document.getElementById('va-pipeline');var sb=document.getElementById('va-save-pipeline-btn');var lb=document.getElementById('va-list-btn');if(pi)pi.disabled=!ok;if(sb)sb.disabled=!ok;if(lb)lb.disabled=!ok;}";
  html += "function vaShowStatus(msg,cls){var d=document.getElementById('va-token-status');if(!d)return;d.style.display=msg?'block':'none';d.textContent=msg;var c={'ok':['#eaf8ee','#155724','#9ad6a7'],'err':['#fdecef','#7a1c26','#f3a7af'],'warn':['#fff5e6','#8a5a00','#f3ca85']};var s=c[cls]||c['warn'];d.style.background=s[0];d.style.color=s[1];d.style.border='1px solid '+s[2];}";
  html += "async function vaSaveHostPort(){var h=document.getElementById('va-host').value.trim();var p=document.getElementById('va-port').value.trim();if(!h){alert('Hote requis');return;}var pn=parseInt(p,10);if(!p||isNaN(pn)||pn<1||pn>65535){alert('Port invalide');return;}try{var r1=await fetch('/post?param=ha_host&value='+encodeURIComponent(h),{method:'POST'});var r2=await fetch('/post?param=ha_port&value='+encodeURIComponent(p),{method:'POST'});if(r1.ok&&r2.ok){vaTokenOk=false;vaShowStatus('','');vaSetPipelineLock(false);alert('Hote et port enregistres.');}else{alert('Erreur enregistrement hote/port.');}}catch(e){alert('Erreur: '+e.message);}}";
  html += "async function vaResetHostPort(){try{await fetch('/delete?param=ha_host',{method:'DELETE'});await fetch('/delete?param=ha_port',{method:'DELETE'});document.getElementById('va-host').value='';document.getElementById('va-port').value='8123';vaTokenOk=false;vaShowStatus('','');vaSetPipelineLock(false);alert('Hote re-initialise.');}catch(e){alert('Erreur: '+e.message);}}";
  html += "async function vaSaveToken(){var t=document.getElementById('va-token').value.trim();if(!t){alert('Token requis');return;}try{var r=await fetch('/post?param=ha_token&value='+encodeURIComponent(t),{method:'POST'});if(r.ok){vaTokenOk=false;vaShowStatus('','');vaSetPipelineLock(false);alert('Token enregistre.');}else{alert('Erreur enregistrement token.');}}catch(e){alert('Erreur: '+e.message);}}";
  html += "async function vaClearToken(){try{var r=await fetch('/delete?param=ha_token',{method:'DELETE'});if(r.ok){document.getElementById('va-token').value='';vaTokenOk=false;vaShowStatus('','');vaSetPipelineLock(false);alert('Token efface.');}else{alert('Erreur effacement token.');}}catch(e){alert('Erreur: '+e.message);}}";
  html += "function vaTestToken(){var host=document.getElementById('va-host').value.trim();var port=document.getElementById('va-port').value.trim();var token=document.getElementById('va-token').value.trim();if(!host||!port||!token){alert('Hote, port et token requis');return;}var btn=document.getElementById('va-test-btn');btn.disabled=true;btn.textContent='Test...';vaShowStatus('Test en cours...','warn');function wsTest(){var ws=new WebSocket('ws://'+host+':'+port+'/api/websocket');var done=false;ws.onmessage=function(ev){var msg;try{msg=JSON.parse(ev.data);}catch(e){return;}if(msg.type==='auth_required'){ws.send(JSON.stringify({type:'auth',access_token:token}));}else if(msg.type==='auth_ok'){done=true;vaTokenOk=true;vaShowStatus('auth_ok - Token valide','ok');vaSetPipelineLock(true);ws.close();}else if(msg.type==='auth_invalid'){done=true;vaTokenOk=false;vaShowStatus('auth_invalid - Token invalide','err');vaSetPipelineLock(false);ws.close();}};ws.onerror=function(){btn.disabled=false;btn.textContent='Tester token';if(!done){vaShowStatus('Erreur connexion WS','err');}};ws.onclose=function(){btn.disabled=false;btn.textContent='Tester token';if(!done){vaShowStatus('Connexion fermee - verifiez hote/port','warn');}};setTimeout(function(){if(ws&&ws.readyState<2){ws.close();if(!done){vaShowStatus('Timeout','warn');btn.disabled=false;btn.textContent='Tester token';}}},5000);}fetch('http://'+host+':'+port+'/api/',{method:'GET',headers:{Authorization:'Bearer '+token}}).then(function(r){if(r.status===200){vaTokenOk=true;vaShowStatus('200 OK - Token valide','ok');vaSetPipelineLock(true);btn.disabled=false;btn.textContent='Tester token';}else if(r.status===401){vaTokenOk=false;vaShowStatus('401 Unauthorized - Token invalide','err');vaSetPipelineLock(false);btn.disabled=false;btn.textContent='Tester token';}else if(r.status===403){vaTokenOk=false;vaShowStatus('403 Forbidden','warn');vaSetPipelineLock(false);btn.disabled=false;btn.textContent='Tester token';}else{wsTest();}}).catch(function(){wsTest();});}";
  html += "async function vaSavePipeline(){if(!vaTokenOk){alert('Teste le token d abord');return;}var pid=document.getElementById('va-pipeline').value.trim();if(!pid){alert('pipeline_id requis');return;}try{var r=await fetch('/post?param=ha_pipeline_id&value='+encodeURIComponent(pid),{method:'POST'});if(r.ok){alert('pipeline_id enregistre.');}else{alert('Erreur enregistrement pipeline_id.');}}catch(e){alert('Erreur: '+e.message);}}";
  html += "function vaListPipelines(){if(!vaTokenOk){alert('Teste le token d abord');return;}var host=document.getElementById('va-host').value.trim();var port=document.getElementById('va-port').value.trim();var token=document.getElementById('va-token').value.trim();var panel=document.getElementById('va-pipeline-list');if(!panel||!host||!port||!token)return;panel.innerHTML='<div class=\"hint\">Connexion HA...</div>';var ws=new WebSocket('ws://'+host+':'+port+'/api/websocket');var done=false;function finish(h){if(done)return;done=true;panel.innerHTML=h;if(ws&&ws.readyState<2)ws.close();}ws.onmessage=function(ev){var msg;try{msg=JSON.parse(ev.data);}catch(e){return;}if(msg.type==='auth_required'){ws.send(JSON.stringify({type:'auth',access_token:token}));}else if(msg.type==='auth_ok'){ws.send(JSON.stringify({id:99,type:'assist_pipeline/pipeline/list'}));}else if(msg.type==='auth_invalid'){finish('<div class=\"hint\" style=\"color:#c00\">Token refuse</div>');}else if(msg.type==='result'&&msg.id===99){if(!msg.success){finish('<div class=\"hint\" style=\"color:#c00\">Erreur HA</div>');return;}var pl=(msg.result&&msg.result.pipelines)||[];if(!pl.length){finish('<div class=\"hint\">Aucun pipeline trouve dans HA</div>');return;}var h0='<div style=\"border:1px solid #c9b8e8;border-radius:6px;overflow:hidden;margin-top:10px\">';h0+='<div style=\"background:#ede0ff;padding:8px 12px;font-size:13px;color:#3d1a78;font-weight:bold\">Cliquez sur Selectionner pour remplir pipeline_id</div>';pl.forEach(function(p,i){var bg=i%2===0?'#fff':'#f8f3ff';h0+='<div style=\"display:flex;align-items:center;justify-content:space-between;padding:8px 12px;background:'+bg+';border-top:1px solid #e4d6f5\">';h0+='<div><div style=\"font-weight:bold\">'+(p.name||'(sans nom)')+'</div>';h0+='<div style=\"font-size:12px;color:#666;font-family:monospace\">'+(p.id||'')+'</div></div>';h0+='<button type=\"button\" class=\"btn-save\" style=\"font-size:12px;padding:6px 12px;margin-top:0\" data-pid=\"'+(p.id||'')+'\" onclick=\"vaSelectPipeline(this.dataset.pid)\">Selectionner</button>';h0+='</div>';});h0+='</div>';finish(h0);}};ws.onerror=function(){finish('<div class=\"hint\" style=\"color:#c00\">Erreur connexion WS</div>');};ws.onclose=function(){if(!done)finish('<div class=\"hint\" style=\"color:#c00\">Connexion fermee</div>');};setTimeout(function(){if(!done)finish('<div class=\"hint\" style=\"color:#c00\">Timeout</div>');},8000);}";
  html += "function vaSelectPipeline(id){var i=document.getElementById('va-pipeline');if(i){i.value=id;}}";
  if (provisioning_mode) {
    html += "scanWifi();";
  }
  html += "</script>";
  html += "</div></div></body></html>";
  return html;
}

String NetworkManager::buildStatusJson() const {
  String json = "{";
  json += "\"portal_running\":";
  json += portal_running_ ? "true" : "false";
  json += ",\"wifi_connected\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  json += ",\"wifi_ip\":\"";
  json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  json += "\",\"ap_ssid\":\"";
  json += provisioningSsid();
  json += "\"}";
  return json;
}

String NetworkManager::buildScanJson() const {
  String json = "{\"networks\":[";
  const int network_count = WiFi.scanNetworks(false, true, false, 300, 0);
  for (int i = 0; i < network_count; ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "{";
    json += "\"ssid\":\"";
    json += WiFi.SSID(i);
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"secure\":";
    json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
    json += "}";
  }
  json += "]}";
  WiFi.scanDelete();
  return json;
}

String NetworkManager::provisioningSsid() const {
  const uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFF);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Snapcast-Setup-%06lX", static_cast<unsigned long>(chip));
  return String(ssid);
}

}  // namespace snapcast

