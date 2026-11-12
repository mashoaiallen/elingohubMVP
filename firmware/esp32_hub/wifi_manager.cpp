#include "wifi_manager.h"

#include <time.h>

#include <algorithm>

#include "config.h"

namespace {
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
}  // namespace

WifiManager::WifiManager()
    : staAttempting_(false),
      staAttemptStartedMs_(0),
      nextStaAttemptMs_(0),
      staRetryDelayMs_(Config::STA_RETRY_MIN_MS),
      staConnected_(false),
      sntpStarted_(false) {}

bool WifiManager::begin(const String &staSsid, const String &staPassword, const String &apPassword) {
  staSsid_ = staSsid;
  staPassword_ = staPassword;

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);          // lower latency for students on the AP
  WiFi.setAutoReconnect(false);  // reconnects are paced by update()

  bool apOk = startAp(apPassword);

  // Captive portal: every DNS name resolves to the Hub, so phones open the student page.
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(53, "*", AP_IP);

  nextStaAttemptMs_ = millis();
  return apOk;
}

bool WifiManager::startAp(const String &apPassword) {
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  const char *pass = apPassword.length() >= 8 ? apPassword.c_str() : nullptr;
  bool ok = WiFi.softAP(Config::AP_SSID, pass, Config::AP_CHANNEL, 0, Config::AP_MAX_CLIENTS);
  Serial.printf("[WiFi] AP \"%s\" %s at %s (%s, max %u clients)\n", Config::AP_SSID, ok ? "up" : "FAILED",
                WiFi.softAPIP().toString().c_str(), pass ? "WPA2" : "open", Config::AP_MAX_CLIENTS);
  return ok;
}

bool WifiManager::restartAp(const String &apPassword) {
  WiFi.softAPdisconnect(false);
  delay(100);
  return startAp(apPassword);
}

void WifiManager::setStaCredentials(const String &ssid, const String &password) {
  staSsid_ = ssid;
  staPassword_ = password;
  staRetryDelayMs_ = Config::STA_RETRY_MIN_MS;
  staAttempting_ = false;
  WiFi.disconnect(false, false);
  nextStaAttemptMs_ = millis();
}

void WifiManager::startStaAttempt(uint32_t now) {
  if (staSsid_.length() == 0) {
    nextStaAttemptMs_ = now + Config::STA_RETRY_MAX_MS;
    Serial.println("[WiFi] No upstream network configured (long-press the button to open setup)");
    return;
  }
  Serial.printf("[WiFi] Connecting to upstream \"%s\"\n", staSsid_.c_str());
  WiFi.disconnect(false, false);
  WiFi.begin(staSsid_.c_str(), staPassword_.c_str());
  staAttempting_ = true;
  staAttemptStartedMs_ = now;
}

void WifiManager::update(uint32_t now) {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != staConnected_.load()) {
    staConnected_ = connected;
    if (connected) {
      Serial.printf("[WiFi] Upstream connected: %s, RSSI %d dBm, channel %u\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI(), WiFi.channel());
      staAttempting_ = false;
      staRetryDelayMs_ = Config::STA_RETRY_MIN_MS;
      if (!sntpStarted_) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        sntpStarted_ = true;
      }
    } else {
      Serial.println("[WiFi] Upstream disconnected");
      nextStaAttemptMs_ = now + Config::STA_RETRY_MIN_MS;
    }
  }
  if (connected) {
    return;
  }

  if (staAttempting_ && now - staAttemptStartedMs_ < Config::STA_CONNECT_TIMEOUT_MS) {
    return;
  }
  if (staAttempting_) {
    staAttempting_ = false;
    Serial.printf("[WiFi] Upstream connect timed out; retry in %lu s\n",
                  static_cast<unsigned long>(staRetryDelayMs_ / 1000));
    nextStaAttemptMs_ = now + staRetryDelayMs_;
    staRetryDelayMs_ = std::min<uint32_t>(staRetryDelayMs_ * 2, Config::STA_RETRY_MAX_MS);
    return;
  }
  if (static_cast<int32_t>(now - nextStaAttemptMs_) >= 0) {
    startStaAttempt(now);
  }
}

bool WifiManager::staConnected() const {
  return staConnected_.load();
}

bool WifiManager::timeSynced() const {
  return time(nullptr) > 1700000000;
}

int8_t WifiManager::staRssi() const {
  return staConnected() ? WiFi.RSSI() : 0;
}

uint8_t WifiManager::channel() const {
  return WiFi.channel();
}

uint8_t WifiManager::stationCount() const {
  return WiFi.softAPgetStationNum();
}

String WifiManager::staIp() const {
  return staConnected() ? WiFi.localIP().toString() : String();
}

String WifiManager::staSsid() const {
  return staSsid_;
}
