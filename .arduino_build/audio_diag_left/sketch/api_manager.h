#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\api_manager.h"
#ifndef ELINGO_API_MANAGER_H
#define ELINGO_API_MANAGER_H

#include <Arduino.h>

struct ApiResult {
  ApiResult() : success(false), httpCode(0), body(""), error("") {}

  bool success;
  int httpCode;
  String body;
  String error;
};

class ApiManager {
 public:
  ApiManager();

  void begin();
  ApiResult checkHealth();
  ApiResult startSession(String &sessionIdOut);
  ApiResult uploadAudioChunk(
      const String &sessionId,
      uint32_t chunkNumber,
      const uint8_t *audioData,
      size_t audioLength);
  ApiResult endSession(const String &sessionId);

 private:
  bool parseOnlineHealth(const String &payload, String &errorOut) const;
  bool parseSessionId(const String &payload, String &sessionIdOut, String &errorOut) const;
  bool parseChunkStored(const String &payload, String &errorOut) const;
  ApiResult makeError(int httpCode, const String &body, const String &error) const;
  String chunkFilename(uint32_t chunkNumber) const;

  String baseUrl_;
};

#endif
