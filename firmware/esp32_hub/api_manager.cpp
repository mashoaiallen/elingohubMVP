#include "api_manager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>

#include "config.h"

namespace {

uint8_t *allocateBytes(size_t length) {
  uint8_t *buffer = nullptr;

#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
  buffer = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer != nullptr) {
    return buffer;
  }
#endif

  return static_cast<uint8_t *>(malloc(length));
}

void releaseBytes(uint8_t *buffer) {
  free(buffer);
}

}  // namespace

ApiManager::ApiManager() : baseUrl_("") {}

void ApiManager::begin() {
  baseUrl_ = Config::API_BASE_URL;
  Serial.print("Backend base URL: ");
  Serial.println(baseUrl_);
}

ApiResult ApiManager::checkHealth() {
  HTTPClient http;
  String url = Config::endpoint("/api/health");

  Serial.print("GET ");
  Serial.println(url);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed for health check");
    return makeError(0, "", "HTTP begin failed for health check");
  }

  http.setTimeout(Config::HTTP_TIMEOUT_MS);
  int statusCode = http.GET();
  String payload = http.getString();
  http.end();

  Serial.print("Health status code: ");
  Serial.println(statusCode);

  if (statusCode != HTTP_CODE_OK) {
    Serial.println(payload);
    return makeError(statusCode, payload, "health check returned non-OK status");
  }

  String parseError;
  bool online = parseOnlineHealth(payload, parseError);
  ApiResult result;
  result.success = online;
  result.httpCode = statusCode;
  result.body = payload;
  if (!online) {
    result.error = parseError;
  }
  return result;
}

ApiResult ApiManager::startSession(String &sessionIdOut) {
  HTTPClient http;
  String url = Config::endpoint("/api/session/start");

  Serial.print("POST ");
  Serial.println(url);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed for session start");
    return makeError(0, "", "HTTP begin failed for session start");
  }

  http.setTimeout(Config::HTTP_TIMEOUT_MS);
  int statusCode = http.POST("");
  String payload = http.getString();
  http.end();

  Serial.print("Start session status code: ");
  Serial.println(statusCode);

  if (statusCode != HTTP_CODE_OK) {
    Serial.println(payload);
    return makeError(statusCode, payload, "session start returned non-OK status");
  }

  String parseError;
  bool parsed = parseSessionId(payload, sessionIdOut, parseError);
  ApiResult result;
  result.success = parsed;
  result.httpCode = statusCode;
  result.body = payload;
  if (!parsed) {
    result.error = parseError;
  }
  return result;
}

ApiResult ApiManager::uploadAudioChunk(
    const String &sessionId,
    uint32_t chunkNumber,
    const uint8_t *audioData,
    size_t audioLength) {
  if (audioData == nullptr || audioLength == 0) {
    Serial.println("Refusing to upload an empty audio chunk");
    return makeError(0, "", "audio chunk is empty");
  }

  HTTPClient http;
  String url = Config::endpoint("/api/session/audio");
  String boundary = "----eLingoHubBoundary";
  boundary += String(millis(), HEX);

  String header;
  header.reserve(256);
  header += "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"sessionId\"\r\n\r\n";
  header += sessionId + "\r\n";
  header += "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"chunkNumber\"\r\n\r\n";
  header += String(chunkNumber) + "\r\n";
  header += "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"audioFile\"; filename=\"";
  header += chunkFilename(chunkNumber);
  header += "\"\r\n";
  header += "Content-Type: audio/wav\r\n\r\n";

  String footer = "\r\n--" + boundary + "--\r\n";
  size_t bodyLength = header.length() + audioLength + footer.length();

  uint8_t *body = allocateBytes(bodyLength);
  if (body == nullptr) {
    Serial.println("Unable to allocate multipart upload body");
    return makeError(0, "", "unable to allocate multipart upload body");
  }

  size_t offset = 0;
  memcpy(body + offset, header.c_str(), header.length());
  offset += header.length();
  memcpy(body + offset, audioData, audioLength);
  offset += audioLength;
  memcpy(body + offset, footer.c_str(), footer.length());

  Serial.print("POST ");
  Serial.println(url);
  Serial.print("Uploading chunk ");
  Serial.print(chunkNumber);
  Serial.print(" bytes: ");
  Serial.println(bodyLength);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed for audio upload");
    releaseBytes(body);
    return makeError(0, "", "HTTP begin failed for audio upload");
  }

  http.setTimeout(Config::HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int statusCode = http.POST(body, bodyLength);
  String payload = http.getString();
  http.end();
  releaseBytes(body);

  Serial.print("Audio upload status code: ");
  Serial.println(statusCode);

  if (statusCode != HTTP_CODE_OK) {
    Serial.println(payload);
    return makeError(statusCode, payload, "audio upload returned non-OK status");
  }

  String parseError;
  bool stored = parseChunkStored(payload, parseError);
  ApiResult result;
  result.success = stored;
  result.httpCode = statusCode;
  result.body = payload;
  if (!stored) {
    result.error = parseError;
  }
  return result;
}

ApiResult ApiManager::endSession(const String &sessionId) {
  if (sessionId.length() == 0) {
    Serial.println("No active session to end");
    return makeError(0, "", "sessionId is empty");
  }

  HTTPClient http;
  String url = Config::endpoint("/api/session/end");

  StaticJsonDocument<128> requestDoc;
  requestDoc["sessionId"] = sessionId;
  String requestBody;
  serializeJson(requestDoc, requestBody);

  Serial.print("POST ");
  Serial.println(url);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed for session end");
    return makeError(0, "", "HTTP begin failed for session end");
  }

  http.setTimeout(Config::HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  int statusCode = http.POST(requestBody);
  String payload = http.getString();
  http.end();

  Serial.print("End session status code: ");
  Serial.println(statusCode);

  if (statusCode != HTTP_CODE_OK) {
    Serial.println(payload);
    return makeError(statusCode, payload, "session end returned non-OK status");
  }

  StaticJsonDocument<128> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, payload);
  if (error) {
    Serial.print("Invalid end-session JSON: ");
    Serial.println(error.c_str());
    return makeError(statusCode, payload, String("invalid end-session JSON: ") + error.c_str());
  }

  const char *message = responseDoc["message"] | "";
  bool ended = String(message) == "Session ended";
  if (!ended) {
    Serial.println("End-session response did not confirm completion");
  }

  ApiResult result;
  result.success = ended;
  result.httpCode = statusCode;
  result.body = payload;
  if (!ended) {
    result.error = "end-session response did not confirm completion";
  }
  return result;
}

bool ApiManager::parseOnlineHealth(const String &payload, String &errorOut) const {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Invalid health JSON: ");
    Serial.println(error.c_str());
    errorOut = String("invalid health JSON: ") + error.c_str();
    return false;
  }

  const char *status = doc["status"] | "";
  bool online = String(status) == "online";
  if (!online) {
    Serial.println("Health response did not report online status");
    errorOut = "health response did not report online status";
  }

  return online;
}

bool ApiManager::parseSessionId(const String &payload, String &sessionIdOut, String &errorOut) const {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Invalid start-session JSON: ");
    Serial.println(error.c_str());
    errorOut = String("invalid start-session JSON: ") + error.c_str();
    return false;
  }

  const char *sessionId = doc["sessionId"] | "";
  if (sessionId[0] == '\0') {
    Serial.println("Start-session response did not include sessionId");
    errorOut = "start-session response did not include sessionId";
    return false;
  }

  sessionIdOut = sessionId;
  return true;
}

bool ApiManager::parseChunkStored(const String &payload, String &errorOut) const {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Invalid audio-upload JSON: ");
    Serial.println(error.c_str());
    errorOut = String("invalid audio-upload JSON: ") + error.c_str();
    return false;
  }

  const char *message = doc["message"] | "";
  const char *path = doc["path"] | "";
  bool stored = String(message) == "Chunk stored" && path[0] != '\0';

  if (!stored) {
    Serial.println("Upload response did not confirm stored chunk");
    errorOut = "upload response did not confirm stored chunk";
  }

  return stored;
}

ApiResult ApiManager::makeError(int httpCode, const String &body, const String &error) const {
  ApiResult result;
  result.success = false;
  result.httpCode = httpCode;
  result.body = body;
  result.error = error;
  return result;
}

String ApiManager::chunkFilename(uint32_t chunkNumber) const {
  char filename[24];
  snprintf(filename, sizeof(filename), "chunk%04lu.wav", static_cast<unsigned long>(chunkNumber));
  return String(filename);
}
