#include "wifi_manager.h"

#include "config.h"

WifiManager::WifiManager()
    : ssid_(nullptr),
      password_(nullptr),
      lastAttemptMs_(0),
      lastStatus_(WL_IDLE_STATUS) {}

void WifiManager::begin(const char *ssid, const char *password) {
  ssid_ = ssid;
  password_ = password;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  startConnection();
}

void WifiManager::update() {
  wl_status_t currentStatus = WiFi.status();
  if (currentStatus != lastStatus_) {
    Serial.print("WiFi status: ");
    Serial.println(statusName(currentStatus));
    lastStatus_ = currentStatus;
  }

  if (currentStatus == WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();
  if (now - lastAttemptMs_ >= Config::WIFI_RETRY_INTERVAL_MS) {
    startConnection();
  }
}

bool WifiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WifiManager::localIp() const {
  if (!isConnected()) {
    return String();
  }

  return WiFi.localIP().toString();
}

void WifiManager::startConnection() {
  lastAttemptMs_ = millis();

  if (ssid_ == nullptr || ssid_[0] == '\0') {
    Serial.println("WiFi SSID is empty. Update config.cpp before flashing.");
    return;
  }

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(ssid_);

  WiFi.disconnect(false, false);
  WiFi.begin(ssid_, password_);
}

const char *WifiManager::statusName(wl_status_t status) const {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "ssid unavailable";
    case WL_SCAN_COMPLETED:
      return "scan completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect failed";
    case WL_CONNECTION_LOST:
      return "connection lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}
