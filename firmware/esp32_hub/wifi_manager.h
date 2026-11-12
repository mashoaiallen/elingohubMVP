#ifndef ELINGO_WIFI_MANAGER_H
#define ELINGO_WIFI_MANAGER_H

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

#include <atomic>

// AP+STA networking.
//   * AP  "eLingo-Hub" at 192.168.4.1 for students (always up, works without internet).
//   * STA joins the upstream hotspot/router for cloud access.
// Both share one radio, so the AP runs on whatever channel the upstream network uses; when
// that changes, the ESP32 moves the AP and announces it (802.11 CSA). STA retries back off
// (5 s -> 60 s) so a missing upstream network does not keep disturbing students.
// The AP does not route students to the internet; they only need the Hub.
class WifiManager {
 public:
  WifiManager();

  bool begin(const String &staSsid, const String &staPassword, const String &apPassword);
  void update(uint32_t now);

  void setStaCredentials(const String &ssid, const String &password);
  bool restartAp(const String &apPassword);

  bool staConnected() const;
  bool timeSynced() const;
  int8_t staRssi() const;
  uint8_t channel() const;
  uint8_t stationCount() const;
  String staIp() const;
  String staSsid() const;

 private:
  void startStaAttempt(uint32_t now);
  bool startAp(const String &apPassword);

  DNSServer dns_;
  String staSsid_;
  String staPassword_;
  bool staAttempting_;
  uint32_t staAttemptStartedMs_;
  uint32_t nextStaAttemptMs_;
  uint32_t staRetryDelayMs_;
  std::atomic<bool> staConnected_;
  bool sntpStarted_;
};

#endif
