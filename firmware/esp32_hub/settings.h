#ifndef ELINGO_SETTINGS_H
#define ELINGO_SETTINGS_H

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct HubSettings {
  String staSsid;
  String staPassword;
  String apPassword;
  String setupPin;
  String supabaseUrl;
  String publishableKey;
  String hubEmail;
  String hubPassword;

  bool cloudConfigured() const {
    return supabaseUrl.length() > 0 && publishableKey.length() > 0 && hubEmail.length() > 0 &&
           hubPassword.length() > 0;
  }
};

// Persistent settings in NVS (namespace "elingo"). Thread-safe: every accessor copies.
class SettingsStore {
 public:
  SettingsStore();

  bool begin();
  HubSettings snapshot() const;

  bool setWifi(const String &ssid, const String &password);
  bool setApPassword(const String &password);
  // Serial-console update; returns false for unknown keys or invalid values.
  bool setField(const String &key, const String &value);
  void factoryReset();
  void printMasked(Print &out) const;

 private:
  void load();
  void seedEmptyFieldsFromSecrets();
  bool writeString(const char *key, const String &value);

  mutable SemaphoreHandle_t mutex_;
  Preferences prefs_;
  HubSettings settings_;
};

#endif
