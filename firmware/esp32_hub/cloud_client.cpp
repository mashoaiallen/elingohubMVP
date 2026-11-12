#include "cloud_client.h"

#include <esp_heap_caps.h>

#include <algorithm>

#include "config.h"

// Mozilla CA bundle compiled into the ESP32 Arduino core (libmbedtls.a, x509_crt_bundle.S).
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {

// Stream sink that collects an HTTP body into a growing PSRAM buffer (handles chunked bodies).
class BufferStream : public Stream {
 public:
  BufferStream(size_t maxBytes, size_t initial) : data_(nullptr), length_(0), capacity_(0), max_(maxBytes), overflow_(false) {
    reserve(initial > 0 ? initial : 64 * 1024);
  }
  ~BufferStream() override { free(data_); }

  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (length_ + size > max_) {
      overflow_ = true;
      return 0;
    }
    if (length_ + size > capacity_ && !reserve(std::max(capacity_ * 2, length_ + size))) {
      overflow_ = true;
      return 0;
    }
    memcpy(data_ + length_, buffer, size);
    length_ += size;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  uint8_t *release(size_t *length) {
    uint8_t *out = data_;
    *length = length_;
    data_ = nullptr;
    length_ = capacity_ = 0;
    return out;
  }
  bool overflowed() const { return overflow_; }

 private:
  bool reserve(size_t wanted) {
    wanted = std::min(wanted, max_);
    void *p = data_ == nullptr ? heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                               : heap_caps_realloc(data_, wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      return false;
    }
    data_ = static_cast<uint8_t *>(p);
    capacity_ = wanted;
    return true;
  }

  uint8_t *data_;
  size_t length_;
  size_t capacity_;
  size_t max_;
  bool overflow_;
};

String trimSlash(String url) {
  while (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  return url;
}

}  // namespace

CloudClient::CloudClient() : tokenExpiresAtMs_(0), tlsReady_(false) {}

void CloudClient::configure(const HubSettings &settings) {
  if (!tlsReady_) {
    tls_.setCACertBundle(x509_crt_bundle_start, x509_crt_bundle_end - x509_crt_bundle_start);
    tls_.setHandshakeTimeout(15);
    http_.setReuse(true);
    http_.setTimeout(Config::HTTP_TIMEOUT_MS);
    http_.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT_MS);
    tlsReady_ = true;
  }
  baseUrl_ = trimSlash(settings.supabaseUrl);
  apiKey_ = settings.publishableKey;
  email_ = settings.hubEmail;
  password_ = settings.hubPassword;
  reset();
}

bool CloudClient::configured() const {
  return baseUrl_.startsWith("https://") && apiKey_.length() > 0 && email_.length() > 0 && password_.length() > 0;
}

bool CloudClient::authenticated() const {
  return accessToken_.length() > 0 && static_cast<int32_t>(tokenExpiresAtMs_ - millis()) > 0;
}

void CloudClient::reset() {
  accessToken_ = "";
  refreshToken_ = "";
  tokenExpiresAtMs_ = 0;
  tls_.stop();
}

CloudResult CloudClient::ensureAuth() {
  // Refresh 2 minutes before expiry.
  if (accessToken_.length() > 0 && static_cast<int32_t>(tokenExpiresAtMs_ - millis()) > 120000) {
    CloudResult ok;
    ok.ok = true;
    return ok;
  }
  if (refreshToken_.length() > 0) {
    CloudResult refreshed = signIn(true);
    if (refreshed.ok) {
      return refreshed;
    }
    Serial.printf("[Cloud] Token refresh failed (%s); signing in again\n", refreshed.error.c_str());
  }
  return signIn(false);
}

CloudResult CloudClient::signIn(bool useRefreshToken) {
  JsonDocument body;
  String url = baseUrl_ + "/auth/v1/token?grant_type=" + (useRefreshToken ? "refresh_token" : "password");
  if (useRefreshToken) {
    body["refresh_token"] = refreshToken_;
  } else {
    body["email"] = email_;
    body["password"] = password_;
  }
  String payload;
  serializeJson(body, payload);

  String response;
  CloudResult result = send("POST", url, "application/json", reinterpret_cast<const uint8_t *>(payload.c_str()),
                            payload.length(), false, &response);
  if (!result.ok) {
    if (!useRefreshToken) {
      accessToken_ = "";
      refreshToken_ = "";
    }
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    result.ok = false;
    result.error = "invalid auth response";
    return result;
  }
  accessToken_ = doc["access_token"] | "";
  refreshToken_ = doc["refresh_token"] | "";
  uint32_t expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAtMs_ = millis() + expiresIn * 1000UL;
  if (accessToken_.length() == 0) {
    result.ok = false;
    result.error = "auth response had no token";
  } else {
    Serial.printf("[Cloud] Signed in (%s), token valid %lu s\n", useRefreshToken ? "refresh" : "password",
                  static_cast<unsigned long>(expiresIn));
  }
  return result;
}

CloudResult CloudClient::rpc(const char *function, const JsonDocument &args, JsonDocument &response) {
  CloudResult auth = ensureAuth();
  if (!auth.ok) {
    return auth;
  }
  String payload;
  serializeJson(args, payload);
  String url = baseUrl_ + "/rest/v1/rpc/" + function;

  String body;
  CloudResult result = send("POST", url, "application/json", reinterpret_cast<const uint8_t *>(payload.c_str()),
                            payload.length(), true, &body);
  if (result.httpCode == 401) {
    accessToken_ = "";  // expired or revoked: sign in again once
    auth = ensureAuth();
    if (!auth.ok) {
      return auth;
    }
    result = send("POST", url, "application/json", reinterpret_cast<const uint8_t *>(payload.c_str()),
                  payload.length(), true, &body);
  }
  if (!result.ok) {
    return result;
  }
  response.clear();
  if (body.length() > 0 && deserializeJson(response, body)) {
    result.ok = false;
    result.error = String("invalid JSON from ") + function;
  }
  return result;
}

CloudResult CloudClient::upload(const char *bucket, const String &path, const uint8_t *data, size_t length,
                                const char *contentType) {
  CloudResult auth = ensureAuth();
  if (!auth.ok) {
    return auth;
  }
  String url = baseUrl_ + "/storage/v1/object/" + bucket + "/" + path;
  // x-upsert makes a retried upload of the same chunk idempotent.
  return send("POST", url, contentType, data, length, true, nullptr, "x-upsert", "true");
}

CloudResult CloudClient::download(const char *bucket, const String &path, size_t maxBytes, uint8_t **data,
                                  size_t *length) {
  *data = nullptr;
  *length = 0;
  CloudResult result = ensureAuth();
  if (!result.ok) {
    return result;
  }
  String url = baseUrl_ + "/storage/v1/object/authenticated/" + bucket + "/" + path;
  if (!http_.begin(tls_, url)) {
    result.ok = false;
    result.error = "http begin failed";
    return result;
  }
  http_.addHeader("apikey", apiKey_);
  http_.addHeader("Authorization", "Bearer " + accessToken_);
  int code = http_.GET();
  result.httpCode = code;
  if (code != HTTP_CODE_OK) {
    String body = code > 0 ? http_.getString() : String();
    http_.end();
    result.ok = false;
    result.error = code > 0 ? String("HTTP ") + code : HTTPClient::errorToString(code);
    parseError(body, result);
    if (code == 401) {
      accessToken_ = "";
    }
    return result;
  }
  int size = http_.getSize();
  if (size > 0 && static_cast<size_t>(size) > maxBytes) {
    http_.end();
    result.ok = false;
    result.error = "segment too large";
    return result;
  }
  BufferStream sink(maxBytes, size > 0 ? static_cast<size_t>(size) : 0);
  int written = http_.writeToStream(&sink);
  http_.end();
  if (written < 0 || sink.overflowed() || (size > 0 && written != size)) {
    result.ok = false;
    result.error = written < 0 ? HTTPClient::errorToString(written) : String("incomplete download");
    return result;
  }
  *data = sink.release(length);
  result.ok = true;
  return result;
}

CloudResult CloudClient::send(const char *method, const String &url, const char *contentType, const uint8_t *body,
                              size_t length, bool withAuth, String *responseBody, const char *extraHeaderName,
                              const char *extraHeaderValue) {
  CloudResult result;
  if (!http_.begin(tls_, url)) {
    result.error = "http begin failed";
    return result;
  }
  http_.addHeader("apikey", apiKey_);
  if (withAuth) {
    http_.addHeader("Authorization", "Bearer " + accessToken_);
  }
  http_.addHeader("Content-Type", contentType);
  if (extraHeaderName != nullptr) {
    http_.addHeader(extraHeaderName, extraHeaderValue);
  }
  int code = http_.sendRequest(method, const_cast<uint8_t *>(body), length);
  result.httpCode = code;
  String response = code > 0 ? http_.getString() : String();
  http_.end();

  if (code <= 0) {
    result.error = HTTPClient::errorToString(code);
    tls_.stop();  // drop a possibly broken keep-alive connection
    return result;
  }
  if (code < 200 || code >= 300) {
    result.error = String("HTTP ") + code;
    parseError(response, result);
    return result;
  }
  result.ok = true;
  if (responseBody != nullptr) {
    *responseBody = response;
  }
  return result;
}

void CloudClient::parseError(const String &body, CloudResult &result) const {
  if (body.length() == 0) {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.error += ": " + body.substring(0, 160);
    return;
  }
  const char *message = doc["message"] | doc["msg"] | doc["error_description"] | doc["error"] | "";
  const char *code = doc["code"] | "";
  if (message[0] != '\0') {
    result.error += String(": ") + message;
  }
  result.pgCode = code;
}
