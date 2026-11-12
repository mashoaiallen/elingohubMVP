#ifndef ELINGO_LOCAL_SERVER_H
#define ELINGO_LOCAL_SERVER_H

#include <Arduino.h>
#include <esp_http_server.h>

#include <atomic>

#include "listener_registry.h"
#include "segment_store.h"
#include "session_manager.h"
#include "settings.h"
#include "wifi_manager.h"

// HTTP server for the student network (http://192.168.4.1), built on ESP-IDF esp_http_server.
//
// Student API (JSON):
//   GET  /api/status                      hub/session/languages/connection state
//   POST /api/join       {listenerId, language}     subscribe or switch language
//   POST /api/heartbeat  {listenerId, playedSeq}    keep-alive (every 10 s)
//   POST /api/leave      {listenerId}
//   GET  /api/segments?lang=af&after=12   recent segments newer than a cursor
//   GET  /api/audio?lang=af&seq=13        audio/wav bytes (each phone keeps its own cursor)
// Owner setup (only while setup mode is on, PIN required):
//   POST /api/setup/login {pin} -> {token};  GET /api/setup/scan;  POST /api/setup/wifi; POST /api/setup/ap
// Everything else: static files from LittleFS (/index.html, /app.js, /app.css, /setup.html);
// unknown paths redirect to "/" so phones' captive-portal checks open the student page.
class LocalServer {
 public:
  LocalServer(SettingsStore &settings, WifiManager &wifi, ListenerRegistry &listeners, SegmentStore &segments,
              SessionManager &session);

  bool begin(bool filesystemMounted);
  void enableSetup(uint32_t durationMs);
  void disableSetup();
  bool setupActive() const;
  void update(uint32_t now);

  // Set by the setup page; consumed by the control loop (applied outside the HTTP task).
  bool takeWifiChanged();
  bool takeApChanged();

 private:
  static esp_err_t handleStatus(httpd_req_t *req);
  static esp_err_t handleJoin(httpd_req_t *req);
  static esp_err_t handleHeartbeat(httpd_req_t *req);
  static esp_err_t handleLeave(httpd_req_t *req);
  static esp_err_t handleSegments(httpd_req_t *req);
  static esp_err_t handleAudio(httpd_req_t *req);
  static esp_err_t handleSetupLogin(httpd_req_t *req);
  static esp_err_t handleSetupStatus(httpd_req_t *req);
  static esp_err_t handleSetupScan(httpd_req_t *req);
  static esp_err_t handleSetupWifi(httpd_req_t *req);
  static esp_err_t handleSetupAp(httpd_req_t *req);
  static esp_err_t handleApiNotFound(httpd_req_t *req);
  static esp_err_t handleStatic(httpd_req_t *req);

  static LocalServer *self(httpd_req_t *req);
  bool allow(httpd_req_t *req, uint8_t cost);
  bool setupAuthorized(httpd_req_t *req);
  uint32_t clientIp(httpd_req_t *req) const;

  SettingsStore &settings_;
  WifiManager &wifi_;
  ListenerRegistry &listeners_;
  SegmentStore &segments_;
  SessionManager &session_;
  httpd_handle_t server_;
  bool fsMounted_;

  std::atomic<uint32_t> setupUntilMs_;
  std::atomic<bool> setupEnabled_;
  char setupToken_[33];
  uint8_t pinFailures_;
  uint32_t pinLockedUntilMs_;
  std::atomic<bool> wifiChanged_;
  std::atomic<bool> apChanged_;

  struct Bucket {
    uint32_t ip;
    float tokens;
    uint32_t lastMs;
  };
  Bucket buckets_[16];
  SemaphoreHandle_t bucketMutex_;
};

#endif
