#include "session_manager.h"

#include <esp_random.h>

#include <cstring>

namespace {

void copyText(char *dst, size_t size, const char *src) {
  strncpy(dst, src != nullptr ? src : "", size - 1);
  dst[size - 1] = '\0';
}

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};

}  // namespace

SessionManager::SessionManager() : mutex_(xSemaphoreCreateMutex()) {
  memset(&state_, 0, sizeof(state_));
  memset(languages_, 0, sizeof(languages_));
  copyText(state_.hubName, sizeof(state_.hubName), "eLingo Hub");
  // Random start so a phone that stayed connected across a Hub reboot never sees a reused number.
  state_.sessionNumber = esp_random() & 0x00FFFFFF;
}

void SessionManager::beginSession(const String &sessionId) {
  Lock lock(mutex_);
  state_.sessionActive = true;
  state_.ending = false;
  state_.sessionNumber++;
  copyText(state_.sessionId, sizeof(state_.sessionId), sessionId.c_str());
  state_.sessionStartedMs = millis();
  state_.translating = false;
  state_.degraded = false;
  for (uint8_t i = 0; i < state_.languageCount; ++i) {
    languages_[i].cloudListeners = 0;
  }
}

void SessionManager::endSession() {
  Lock lock(mutex_);
  if (state_.sessionActive) {
    state_.hasEnded = true;
    state_.endedAtMs = millis();
  }
  state_.sessionActive = false;
  state_.ending = false;
  state_.sessionNumber++;  // phones drop their cursor even if they never see the next session start
  state_.sessionId[0] = '\0';
  state_.translating = false;
  state_.degraded = false;
  for (uint8_t i = 0; i < state_.languageCount; ++i) {
    languages_[i].cloudListeners = 0;
  }
}

bool SessionManager::hasActiveSession() const {
  Lock lock(mutex_);
  return state_.sessionActive;
}

uint32_t SessionManager::sessionNumber() const {
  Lock lock(mutex_);
  return state_.sessionNumber;
}

void SessionManager::setEnding(bool value) {
  Lock lock(mutex_);
  state_.ending = value && state_.sessionActive;
}

String SessionManager::sessionId() const {
  Lock lock(mutex_);
  return String(state_.sessionId);
}

void SessionManager::setHub(const char *id, const char *name) {
  Lock lock(mutex_);
  copyText(state_.hubId, sizeof(state_.hubId), id);
  if (name != nullptr && name[0] != '\0') {
    copyText(state_.hubName, sizeof(state_.hubName), name);
  }
}

String SessionManager::hubId() const {
  Lock lock(mutex_);
  return String(state_.hubId);
}

void SessionManager::setCatalog(JsonArrayConst languages) {
  Lock lock(mutex_);
  uint8_t count = 0;
  for (JsonObjectConst lang : languages) {
    if (count >= Config::MAX_LANGUAGES) {
      break;
    }
    LanguageInfo &info = languages_[count];
    copyText(info.code, sizeof(info.code), lang["code"] | "");
    copyText(info.name, sizeof(info.name), lang["name"] | "");
    copyText(info.nativeName, sizeof(info.nativeName), lang["nativeName"] | "");
    info.cloudListeners = 0;
    if (info.code[0] != '\0') {
      ++count;
    }
  }
  state_.languageCount = count;
}

uint8_t SessionManager::catalog(LanguageInfo *out, uint8_t max) const {
  Lock lock(mutex_);
  uint8_t n = state_.languageCount < max ? state_.languageCount : max;
  memcpy(out, languages_, n * sizeof(LanguageInfo));
  return n;
}

bool SessionManager::isKnownLanguage(const char *code) const {
  Lock lock(mutex_);
  for (uint8_t i = 0; i < state_.languageCount; ++i) {
    if (strcmp(languages_[i].code, code) == 0) {
      return true;
    }
  }
  return false;
}

void SessionManager::setCloudCounts(JsonArrayConst counts) {
  Lock lock(mutex_);
  for (uint8_t i = 0; i < state_.languageCount; ++i) {
    languages_[i].cloudListeners = 0;
    for (JsonObjectConst c : counts) {
      if (strcmp(languages_[i].code, c["code"] | "") == 0) {
        languages_[i].cloudListeners = c["listeners"] | 0;
      }
    }
  }
}

void SessionManager::setTranslating(bool value) {
  Lock lock(mutex_);
  state_.translating = value;
}

void SessionManager::setDegraded(bool value) {
  Lock lock(mutex_);
  state_.degraded = value;
}

void SessionManager::setCloudOnline(bool value) {
  Lock lock(mutex_);
  state_.cloudOnline = value;
}

void SessionManager::setSetupMode(bool value) {
  Lock lock(mutex_);
  state_.setupMode = value;
}

HubSnapshot SessionManager::snapshot() const {
  Lock lock(mutex_);
  return state_;
}
