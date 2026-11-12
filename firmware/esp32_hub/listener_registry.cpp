#include "listener_registry.h"

#include <cctype>
#include <cstring>

namespace {
class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};
}  // namespace

ListenerRegistry::ListenerRegistry() : mutex_(xSemaphoreCreateMutex()), version_(0) {
  memset(entries_, 0, sizeof(entries_));
}

bool ListenerRegistry::validId(const char *id) {
  if (id == nullptr) {
    return false;
  }
  size_t len = strlen(id);
  if (len < 8 || len > 64) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    char c = id[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

int ListenerRegistry::indexOf(const char *id) const {
  for (int i = 0; i < Config::MAX_LISTENERS; ++i) {
    if (entries_[i].used && strcmp(entries_[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

JoinResult ListenerRegistry::join(const char *id, const char *lang, uint32_t ip, uint32_t now) {
  if (!validId(id)) {
    return JoinResult::INVALID_ID;
  }
  Lock lock(mutex_);
  int i = indexOf(id);
  if (i < 0) {
    for (int j = 0; j < Config::MAX_LISTENERS; ++j) {
      if (!entries_[j].used) {
        i = j;
        break;
      }
    }
    if (i < 0) {
      return JoinResult::FULL;
    }
    ListenerEntry &e = entries_[i];
    memset(&e, 0, sizeof(e));
    e.used = true;
    strncpy(e.id, id, sizeof(e.id) - 1);
    e.playedSeq = -1;
    ++version_;
  }
  ListenerEntry &e = entries_[i];
  if (strncmp(e.lang, lang, sizeof(e.lang)) != 0) {
    strncpy(e.lang, lang, sizeof(e.lang) - 1);
    e.lang[sizeof(e.lang) - 1] = '\0';
    ++version_;
  }
  e.lastSeenMs = now;
  e.ip = ip;
  return JoinResult::OK;
}

bool ListenerRegistry::heartbeat(const char *id, int32_t playedSeq, uint32_t ip, uint32_t now) {
  Lock lock(mutex_);
  int i = indexOf(id);
  if (i < 0) {
    return false;
  }
  entries_[i].lastSeenMs = now;
  entries_[i].ip = ip;
  if (playedSeq >= 0) {
    entries_[i].playedSeq = playedSeq;
  }
  return true;
}

bool ListenerRegistry::leave(const char *id) {
  Lock lock(mutex_);
  int i = indexOf(id);
  if (i < 0) {
    return false;
  }
  entries_[i].used = false;
  ++version_;
  return true;
}

uint8_t ListenerRegistry::expire(uint32_t now) {
  Lock lock(mutex_);
  uint8_t removed = 0;
  for (ListenerEntry &e : entries_) {
    if (e.used && now - e.lastSeenMs > Config::LISTENER_TIMEOUT_MS) {
      Serial.printf("[Listeners] %s (%s) timed out\n", e.id, e.lang);
      e.used = false;
      ++removed;
    }
  }
  if (removed > 0) {
    ++version_;
  }
  return removed;
}

void ListenerRegistry::clear() {
  Lock lock(mutex_);
  memset(entries_, 0, sizeof(entries_));
  ++version_;
}

uint32_t ListenerRegistry::version() const {
  Lock lock(mutex_);
  return version_;
}

uint8_t ListenerRegistry::count() const {
  Lock lock(mutex_);
  uint8_t n = 0;
  for (const ListenerEntry &e : entries_) {
    n += e.used ? 1 : 0;
  }
  return n;
}

uint8_t ListenerRegistry::countFor(const char *lang) const {
  Lock lock(mutex_);
  uint8_t n = 0;
  for (const ListenerEntry &e : entries_) {
    n += (e.used && strcmp(e.lang, lang) == 0) ? 1 : 0;
  }
  return n;
}

bool ListenerRegistry::find(const char *id, char *langOut, size_t langSize) const {
  Lock lock(mutex_);
  int i = indexOf(id);
  if (i < 0) {
    return false;
  }
  strncpy(langOut, entries_[i].lang, langSize - 1);
  langOut[langSize - 1] = '\0';
  return true;
}

void ListenerRegistry::toJson(JsonArray out) const {
  Lock lock(mutex_);
  for (const ListenerEntry &e : entries_) {
    if (e.used) {
      JsonObject o = out.add<JsonObject>();
      o["id"] = e.id;
      o["lang"] = e.lang;
    }
  }
}

uint8_t ListenerRegistry::activeLanguages(char out[][8], uint8_t max) const {
  Lock lock(mutex_);
  uint8_t n = 0;
  for (const ListenerEntry &e : entries_) {
    if (!e.used) {
      continue;
    }
    bool seen = false;
    for (uint8_t k = 0; k < n; ++k) {
      if (strcmp(out[k], e.lang) == 0) {
        seen = true;
        break;
      }
    }
    if (!seen && n < max) {
      strncpy(out[n], e.lang, 7);
      out[n][7] = '\0';
      ++n;
    }
  }
  return n;
}
