#ifndef ELINGO_CLOUD_CLIENT_H
#define ELINGO_CLOUD_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "settings.h"

struct CloudResult {
  bool ok = false;
  int httpCode = 0;
  String error;   // human readable
  String pgCode;  // PostgREST / Postgres error code when available (e.g. P0002)
};

// Supabase over HTTPS. Not thread-safe: used only by the cloud task.
//   * Auth: the Hub signs in as its own Supabase Auth user (password grant) and refreshes the
//     token before expiry (refresh tokens rotate, the new one is kept in RAM).
//   * Data: PostgREST RPCs (hub_*), scoped by RLS / ownership checks to this Hub.
//   * Audio: Storage upload/download on this Hub's folder.
// One TLS connection is reused (keep-alive) to avoid a handshake per request.
class CloudClient {
 public:
  CloudClient();

  void configure(const HubSettings &settings);
  bool configured() const;
  bool authenticated() const;

  CloudResult ensureAuth();
  CloudResult rpc(const char *function, const JsonDocument &args, JsonDocument &response);
  CloudResult upload(const char *bucket, const String &path, const uint8_t *data, size_t length,
                     const char *contentType);
  // On success *data is a PSRAM buffer owned by the caller (free()).
  CloudResult download(const char *bucket, const String &path, size_t maxBytes, uint8_t **data, size_t *length);
  void reset();

 private:
  CloudResult signIn(bool useRefreshToken);
  CloudResult send(const char *method, const String &url, const char *contentType, const uint8_t *body,
                   size_t length, bool withAuth, String *responseBody, const char *extraHeaderName = nullptr,
                   const char *extraHeaderValue = nullptr);
  void parseError(const String &body, CloudResult &result) const;

  NetworkClientSecure tls_;
  HTTPClient http_;
  String baseUrl_;
  String apiKey_;
  String email_;
  String password_;
  String accessToken_;
  String refreshToken_;
  uint32_t tokenExpiresAtMs_;
  bool tlsReady_;
};

#endif
