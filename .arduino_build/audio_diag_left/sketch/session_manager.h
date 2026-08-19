#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\session_manager.h"
#ifndef ELINGO_SESSION_MANAGER_H
#define ELINGO_SESSION_MANAGER_H

#include <Arduino.h>

class SessionManager {
 public:
  SessionManager();

  void beginSession(const String &sessionId);
  void reset();
  bool hasActiveSession() const;

  const String &sessionId() const;
  uint32_t chunkNumber() const;
  void advanceChunk();

 private:
  String sessionId_;
  uint32_t chunkNumber_;
  bool active_;
};

#endif
