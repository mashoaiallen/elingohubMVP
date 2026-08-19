#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\session_manager.cpp"
#include "session_manager.h"

SessionManager::SessionManager()
    : sessionId_(""),
      chunkNumber_(1),
      active_(false) {}

void SessionManager::beginSession(const String &sessionId) {
  sessionId_ = sessionId;
  chunkNumber_ = 1;
  active_ = true;

  Serial.print("Session active: ");
  Serial.println(sessionId_);
}

void SessionManager::reset() {
  sessionId_ = "";
  chunkNumber_ = 1;
  active_ = false;
}

bool SessionManager::hasActiveSession() const {
  return active_ && sessionId_.length() > 0;
}

const String &SessionManager::sessionId() const {
  return sessionId_;
}

uint32_t SessionManager::chunkNumber() const {
  return chunkNumber_;
}

void SessionManager::advanceChunk() {
  ++chunkNumber_;
}
