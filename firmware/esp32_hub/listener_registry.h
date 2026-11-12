#ifndef ELINGO_LISTENER_REGISTRY_H
#define ELINGO_LISTENER_REGISTRY_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

// Students connected to this Hub. Each phone keeps a random listener id in localStorage, so a
// refresh or reconnect re-uses the same entry (no duplicate subscriptions). Entries expire
// when heartbeats stop (LISTENER_TIMEOUT_MS). The cloud receives the full set every few seconds.
enum class JoinResult : uint8_t { OK, INVALID_ID, UNKNOWN_LANGUAGE, FULL };

struct ListenerEntry {
  bool used;
  char id[65];
  char lang[8];
  uint32_t lastSeenMs;
  uint32_t ip;
  int32_t playedSeq;
};

class ListenerRegistry {
 public:
  ListenerRegistry();

  static bool validId(const char *id);

  JoinResult join(const char *id, const char *lang, uint32_t ip, uint32_t now);
  bool heartbeat(const char *id, int32_t playedSeq, uint32_t ip, uint32_t now);  // false = unknown id
  bool leave(const char *id);
  uint8_t expire(uint32_t now);
  void clear();

  uint32_t version() const;  // increments on any membership/language change
  uint8_t count() const;
  uint8_t countFor(const char *lang) const;
  bool find(const char *id, char *langOut, size_t langSize) const;
  void toJson(JsonArray out) const;  // [{"id": ..., "lang": ...}]
  uint8_t activeLanguages(char out[][8], uint8_t max) const;

 private:
  int indexOf(const char *id) const;

  mutable SemaphoreHandle_t mutex_;
  ListenerEntry entries_[Config::MAX_LISTENERS];
  uint32_t version_;
};

#endif
