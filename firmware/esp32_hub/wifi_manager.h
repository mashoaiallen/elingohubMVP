#ifndef ELINGO_WIFI_MANAGER_H
#define ELINGO_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

class WifiManager {
 public:
  WifiManager();

  void begin(const char *ssid, const char *password);
  void update();

  bool isConnected() const;
  String localIp() const;

 private:
  void startConnection();
  const char *statusName(wl_status_t status) const;

  const char *ssid_;
  const char *password_;
  uint32_t lastAttemptMs_;
  wl_status_t lastStatus_;
};

#endif
