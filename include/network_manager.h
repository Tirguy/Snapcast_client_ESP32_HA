#pragma once

#include <DNSServer.h>
#include <WebServer.h>

#include <stdint.h>

namespace snapcast {

class SnapcastClient;

class NetworkManager {
 public:
  bool begin();
  void tick();

 private:
  bool configureWifi();
    bool beginStationConnection();
  void maintainWifi();
  void maintainSnapcastClient();
    void maintainProvisioningPortal();
    void maintainResetButton();
    bool resetWifiCredentialsAndStartProvisioning();
    void startProvisioningPortal();
    void stopProvisioningPortal();
    void handlePortalRoot();
    void handlePortalSave();
    void handlePortalStatus();
    void handlePortalScan();
    String buildPortalHtml(bool provisioning_mode) const;
    String buildStatusJson() const;
    String buildScanJson() const;
    String provisioningSsid() const;
    void handleGetParam();
    void handlePostParam();
    void handleDeleteParam();

  uint32_t last_wifi_attempt_ms_ = 0;
  uint32_t last_server_attempt_ms_ = 0;
    uint32_t wifi_connect_started_ms_ = 0;
  uint32_t portal_redirect_started_ms_ = 0;
  uint32_t reset_button_guard_started_ms_ = 0;
  uint32_t reset_button_pressed_since_ms_ = 0;
  uint32_t reboot_request_ms_ = 0;
  bool reset_button_initialized_ = false;
  bool reset_button_action_latched_ = false;
  bool portal_start_requested_ = false;
  bool portal_redirect_pending_ = false;
  bool reboot_pending_after_reset_ = false;
  bool config_server_started_ = false;
    bool portal_running_ = false;
    DNSServer dns_server_;
    WebServer portal_server_{80};
};

}  // namespace snapcast
