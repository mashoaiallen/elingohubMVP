#ifndef ELINGO_SESSION_MANAGER_H
#define ELINGO_SESSION_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

struct LanguageInfo {
  char code[8];
  char name[40];
  char nativeName[32];
  uint8_t cloudListeners;  // count confirmed by the cloud (all of this Hub's listeners)
};

struct HubSnapshot {
  bool sessionActive;
  bool ending;              // end requested (button or cloud); final audio still being flushed
  uint32_t sessionNumber;   // changes with every session; phones reset their audio cursor on change
  bool hasEnded;            // a session has ended since boot (endedAtMs is valid)
  uint32_t endedAtMs;
  char sessionId[40];
  uint32_t sessionStartedMs;
  bool translating;   // at least one translated segment has arrived this session
  bool degraded;      // session running but uploads/cloud failing
  bool cloudOnline;
  bool setupMode;
  char hubId[40];
  char hubName[48];
  uint8_t languageCount;
};

// Shared state between the control loop, the cloud task and the web server. All methods are
// thread-safe (one mutex, short critical sections).
class SessionManager {
 public:
  SessionManager();

  void beginSession(const String &sessionId);
  void endSession();
  bool hasActiveSession() const;
  uint32_t sessionNumber() const;
  String sessionId() const;

  void setHub(const char *id, const char *name);
  String hubId() const;

  void setCatalog(JsonArrayConst languages);
  uint8_t catalog(LanguageInfo *out, uint8_t max) const;
  bool isKnownLanguage(const char *code) const;
  void setCloudCounts(JsonArrayConst counts);

  void setTranslating(bool value);
  void setEnding(bool value);
  void setDegraded(bool value);
  void setCloudOnline(bool value);
  void setSetupMode(bool value);

  HubSnapshot snapshot() const;

 private:
  mutable SemaphoreHandle_t mutex_;
  HubSnapshot state_;
  LanguageInfo languages_[Config::MAX_LANGUAGES];
};

#endif
