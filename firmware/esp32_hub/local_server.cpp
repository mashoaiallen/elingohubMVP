#include "local_server.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstring>

#include "config.h"

namespace {

constexpr size_t MAX_JSON_BODY = 1024;
constexpr float BUCKET_CAPACITY = 40.0f;
constexpr float BUCKET_REFILL_PER_S = 12.0f;

const char FALLBACK_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>eLingo Hub</title></head><body style='font-family:sans-serif;padding:16px'>"
    "<h1>eLingo Hub</h1><p>The student page files are missing from this Hub. "
    "Flash the web files (tools/flash-web.mjs) and reload.</p></body></html>";

esp_err_t sendJson(httpd_req_t *req, const JsonDocument &doc, const char *status = "200 OK") {
  String body;
  serializeJson(doc, body);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}

esp_err_t sendError(httpd_req_t *req, const char *status, const char *code, const char *message) {
  JsonDocument doc;
  doc["error"] = code;
  doc["message"] = message;
  return sendJson(req, doc, status);
}

bool readJsonBody(httpd_req_t *req, JsonDocument &doc) {
  if (req->content_len == 0 || req->content_len > MAX_JSON_BODY) {
    return false;
  }
  char buffer[MAX_JSON_BODY + 1];
  size_t received = 0;
  while (received < req->content_len) {
    int r = httpd_req_recv(req, buffer + received, req->content_len - received);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return false;
    }
    received += r;
  }
  buffer[received] = '\0';
  return !deserializeJson(doc, buffer, received);
}

bool queryValue(httpd_req_t *req, const char *key, char *out, size_t size) {
  char query[160];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(query, key, out, size) == ESP_OK;
}

const char *contentTypeFor(const String &path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".json")) return "application/json";
  return "application/octet-stream";
}

bool constantTimeEquals(const String &a, const String &b) {
  if (a.length() != b.length() || a.length() == 0) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace

LocalServer::LocalServer(SettingsStore &settings, WifiManager &wifi, ListenerRegistry &listeners,
                         SegmentStore &segments, SessionManager &session)
    : settings_(settings),
      wifi_(wifi),
      listeners_(listeners),
      segments_(segments),
      session_(session),
      server_(nullptr),
      fsMounted_(false),
      setupUntilMs_(0),
      setupEnabled_(false),
      pinFailures_(0),
      pinLockedUntilMs_(0),
      wifiChanged_(false),
      apChanged_(false),
      bucketMutex_(xSemaphoreCreateMutex()) {
  setupToken_[0] = '\0';
  memset(buckets_, 0, sizeof(buckets_));
}

LocalServer *LocalServer::self(httpd_req_t *req) {
  return static_cast<LocalServer *>(req->user_ctx);
}

bool LocalServer::begin(bool filesystemMounted) {
  fsMounted_ = filesystemMounted;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 20;
  config.max_open_sockets = 9;  // LWIP has 16; leaves room for TLS, DNS and SNTP
  config.lru_purge_enable = true;
  config.stack_size = 8192;
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 10;
  config.core_id = 0;

  if (httpd_start(&server_, &config) != ESP_OK) {
    Serial.println("[Web] httpd_start failed");
    return false;
  }

  struct Route {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
  };
  const Route routes[] = {
      {"/api/status", HTTP_GET, handleStatus},
      {"/api/join", HTTP_POST, handleJoin},
      {"/api/heartbeat", HTTP_POST, handleHeartbeat},
      {"/api/leave", HTTP_POST, handleLeave},
      {"/api/segments", HTTP_GET, handleSegments},
      {"/api/audio", HTTP_GET, handleAudio},
      {"/api/setup/status", HTTP_GET, handleSetupStatus},
      {"/api/setup/login", HTTP_POST, handleSetupLogin},
      {"/api/setup/scan", HTTP_GET, handleSetupScan},
      {"/api/setup/wifi", HTTP_POST, handleSetupWifi},
      {"/api/setup/ap", HTTP_POST, handleSetupAp},
      {"/api/*", HTTP_GET, handleApiNotFound},
      {"/api/*", HTTP_POST, handleApiNotFound},
      {"/*", HTTP_GET, handleStatic},
  };
  for (const Route &r : routes) {
    httpd_uri_t uri = {};
    uri.uri = r.uri;
    uri.method = r.method;
    uri.handler = r.handler;
    uri.user_ctx = this;
    httpd_register_uri_handler(server_, &uri);
  }
  Serial.printf("[Web] Serving http://192.168.4.1 (%s)\n", fsMounted_ ? "LittleFS" : "fallback page only");
  return true;
}

void LocalServer::enableSetup(uint32_t durationMs) {
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  for (int i = 0; i < 16; ++i) {
    snprintf(setupToken_ + i * 2, 3, "%02x", raw[i]);
  }
  pinFailures_ = 0;
  setupUntilMs_ = millis() + durationMs;
  setupEnabled_ = true;
  session_.setSetupMode(true);
  Serial.println("[Web] Setup mode ON: open http://192.168.4.1/setup.html");
}

void LocalServer::disableSetup() {
  setupEnabled_ = false;
  setupToken_[0] = '\0';
  session_.setSetupMode(false);
  Serial.println("[Web] Setup mode OFF");
}

bool LocalServer::setupActive() const {
  return setupEnabled_;
}

void LocalServer::update(uint32_t now) {
  if (setupEnabled_ && static_cast<int32_t>(now - setupUntilMs_) >= 0) {
    disableSetup();
  }
}

bool LocalServer::takeWifiChanged() {
  return wifiChanged_.exchange(false);
}

bool LocalServer::takeApChanged() {
  return apChanged_.exchange(false);
}

uint32_t LocalServer::clientIp(httpd_req_t *req) const {
  int fd = httpd_req_to_sockfd(req);
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
    return 0;
  }
  if (addr.ss_family == AF_INET) {
    return reinterpret_cast<struct sockaddr_in *>(&addr)->sin_addr.s_addr;
  }
#if LWIP_IPV6
  if (addr.ss_family == AF_INET6) {
    // IPv4-mapped IPv6: last 4 bytes are the IPv4 address
    const uint8_t *b = reinterpret_cast<struct sockaddr_in6 *>(&addr)->sin6_addr.s6_addr;
    uint32_t v;
    memcpy(&v, b + 12, 4);
    return v;
  }
#endif
  return 0;
}

bool LocalServer::allow(httpd_req_t *req, uint8_t cost) {
  uint32_t ip = clientIp(req);
  uint32_t now = millis();
  xSemaphoreTake(bucketMutex_, portMAX_DELAY);
  Bucket *bucket = nullptr;
  Bucket *oldest = &buckets_[0];
  for (Bucket &b : buckets_) {
    if (b.ip == ip && b.lastMs != 0) {
      bucket = &b;
      break;
    }
    if (b.lastMs < oldest->lastMs) {
      oldest = &b;
    }
  }
  if (bucket == nullptr) {
    bucket = oldest;
    bucket->ip = ip;
    bucket->tokens = BUCKET_CAPACITY;
    bucket->lastMs = now;
  }
  float elapsed = (now - bucket->lastMs) / 1000.0f;
  bucket->tokens = std::min(BUCKET_CAPACITY, bucket->tokens + elapsed * BUCKET_REFILL_PER_S);
  bucket->lastMs = now == 0 ? 1 : now;
  bool ok = bucket->tokens >= cost;
  if (ok) {
    bucket->tokens -= cost;
  }
  xSemaphoreGive(bucketMutex_);
  if (!ok) {
    sendError(req, "429 Too Many Requests", "rate_limited", "Too many requests; slow down.");
  }
  return ok;
}

// ---------------------------------------------------------------------------------- student API

esp_err_t LocalServer::handleStatus(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;
  HubSnapshot snap = s->session_.snapshot();
  LanguageInfo langs[Config::MAX_LANGUAGES];
  uint8_t n = s->session_.catalog(langs, Config::MAX_LANGUAGES);

  JsonDocument doc;
  doc["hub"]["name"] = snap.hubName;
  doc["hub"]["firmware"] = Config::FIRMWARE_VERSION;
  doc["session"]["active"] = snap.sessionActive;
  doc["session"]["number"] = snap.sessionNumber;
  doc["session"]["ending"] = snap.ending;
  if (!snap.sessionActive && snap.hasEnded) {
    doc["session"]["endedAgoS"] = (millis() - snap.endedAtMs) / 1000;
  } else {
    doc["session"]["endedAgoS"] = nullptr;
  }
  doc["session"]["translating"] = snap.translating;
  doc["session"]["degraded"] = snap.degraded;
  doc["session"]["elapsedS"] = snap.sessionActive ? (millis() - snap.sessionStartedMs) / 1000 : 0;
  doc["cloud"]["online"] = snap.cloudOnline;
  doc["upstream"]["connected"] = s->wifi_.staConnected();
  doc["network"]["clients"] = s->wifi_.stationCount();
  doc["network"]["maxClients"] = Config::AP_MAX_CLIENTS;
  doc["setupMode"] = snap.setupMode;
  JsonArray arr = doc["languages"].to<JsonArray>();
  for (uint8_t i = 0; i < n; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["code"] = langs[i].code;
    o["name"] = langs[i].name;
    o["nativeName"] = langs[i].nativeName;
    o["listeners"] = std::max<uint8_t>(langs[i].cloudListeners, s->listeners_.countFor(langs[i].code));
    o["latestSeq"] = s->segments_.latestSeq(langs[i].code);
  }
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleJoin(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 2)) return ESP_OK;
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  const char *id = body["listenerId"] | "";
  const char *lang = body["language"] | "";
  if (!s->session_.isKnownLanguage(lang)) {
    return sendError(req, "400 Bad Request", "unknown_language", "That language is not available.");
  }
  JoinResult r = s->listeners_.join(id, lang, s->clientIp(req), millis());
  if (r == JoinResult::INVALID_ID) {
    return sendError(req, "400 Bad Request", "invalid_listener", "Invalid listener id.");
  }
  if (r == JoinResult::FULL) {
    return sendError(req, "503 Service Unavailable", "hub_full", "This Hub has reached its listener limit.");
  }
  Serial.printf("[Listeners] %s joined %s (%u total)\n", id, lang, s->listeners_.count());
  JsonDocument doc;
  doc["listenerId"] = id;
  doc["language"] = lang;
  doc["sessionActive"] = s->session_.hasActiveSession();
  doc["latestSeq"] = s->segments_.latestSeq(lang);
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleHeartbeat(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  const char *id = body["listenerId"] | "";
  int32_t played = body["playedSeq"] | -1;
  if (!s->listeners_.heartbeat(id, played, s->clientIp(req), millis())) {
    // Unknown (expired, or the Hub restarted): the page re-joins automatically.
    return sendError(req, "404 Not Found", "unknown_listener", "Please join again.");
  }
  char lang[8];
  s->listeners_.find(id, lang, sizeof(lang));
  JsonDocument doc;
  doc["ok"] = true;
  doc["language"] = lang;
  doc["sessionActive"] = s->session_.hasActiveSession();
  doc["latestSeq"] = s->segments_.latestSeq(lang);
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleLeave(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  const char *id = body["listenerId"] | "";
  bool removed = s->listeners_.leave(id);
  if (removed) {
    Serial.printf("[Listeners] %s left (%u total)\n", id, s->listeners_.count());
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["removed"] = removed;
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleSegments(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;
  char lang[8] = "";
  char after[12] = "-1";
  queryValue(req, "lang", lang, sizeof(lang));
  queryValue(req, "after", after, sizeof(after));
  JsonDocument doc;
  doc["language"] = lang;
  // Sequence numbers restart with every session, so the page resets its cursor when this changes.
  doc["session"] = s->session_.sessionNumber();
  doc["sessionActive"] = s->session_.hasActiveSession();
  doc["latestSeq"] = s->segments_.latestSeq(lang);
  s->segments_.list(lang, atoi(after), doc["segments"].to<JsonArray>());
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleAudio(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;
  char lang[8] = "";
  char seqText[12] = "";
  if (!queryValue(req, "lang", lang, sizeof(lang)) || !queryValue(req, "seq", seqText, sizeof(seqText))) {
    return sendError(req, "400 Bad Request", "bad_request", "lang and seq are required.");
  }
  // seq restarts every session: a request carrying another session's number (a phone that has not
  // noticed the change yet) must not get this session's audio under the same seq.
  char sessionText[12] = "";
  if (queryValue(req, "session", sessionText, sizeof(sessionText)) &&
      strtoul(sessionText, nullptr, 10) != s->session_.sessionNumber()) {
    return sendError(req, "404 Not Found", "segment_gone", "That audio belongs to another session.");
  }
  Segment *segment = s->segments_.acquire(lang, static_cast<uint32_t>(strtoul(seqText, nullptr, 10)));
  if (segment == nullptr) {
    return sendError(req, "404 Not Found", "segment_gone", "That audio segment is no longer buffered.");
  }
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = ESP_OK;
  const size_t step = 8192;
  for (size_t off = 0; off < segment->length && err == ESP_OK; off += step) {
    size_t n = std::min(step, segment->length - off);
    err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(segment->data + off), n);
  }
  if (err == ESP_OK) {
    err = httpd_resp_send_chunk(req, nullptr, 0);
  }
  s->segments_.release(segment);
  return err;
}

// ---------------------------------------------------------------------------------- setup API

bool LocalServer::setupAuthorized(httpd_req_t *req) {
  if (!setupEnabled_) {
    sendError(req, "403 Forbidden", "setup_closed", "Hold the Hub button for 5 seconds to open setup.");
    return false;
  }
  char token[40] = "";
  if (httpd_req_get_hdr_value_str(req, "X-Setup-Token", token, sizeof(token)) != ESP_OK ||
      !constantTimeEquals(String(token), String(setupToken_))) {
    sendError(req, "401 Unauthorized", "setup_login", "Enter the setup PIN.");
    return false;
  }
  return true;
}

esp_err_t LocalServer::handleSetupStatus(httpd_req_t *req) {
  LocalServer *s = self(req);
  JsonDocument doc;
  doc["open"] = s->setupEnabled_.load();
  doc["remainingS"] = s->setupEnabled_ ? (s->setupUntilMs_ - millis()) / 1000 : 0;
  doc["upstream"]["ssid"] = s->wifi_.staSsid();
  doc["upstream"]["connected"] = s->wifi_.staConnected();
  doc["upstream"]["rssi"] = s->wifi_.staRssi();
  doc["upstream"]["ip"] = s->wifi_.staIp();
  doc["cloud"]["online"] = s->session_.snapshot().cloudOnline;
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleSetupLogin(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 5)) return ESP_OK;
  if (!s->setupEnabled_) {
    return sendError(req, "403 Forbidden", "setup_closed", "Hold the Hub button for 5 seconds to open setup.");
  }
  if (static_cast<int32_t>(millis() - s->pinLockedUntilMs_) < 0) {
    return sendError(req, "429 Too Many Requests", "locked", "Too many wrong PINs; wait a minute.");
  }
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  String pin = body["pin"] | "";
  HubSettings settings = s->settings_.snapshot();
  if (settings.setupPin.length() == 0 || !constantTimeEquals(pin, settings.setupPin)) {
    if (++s->pinFailures_ >= 5) {
      s->pinFailures_ = 0;
      s->pinLockedUntilMs_ = millis() + 60000;
    }
    return sendError(req, "401 Unauthorized", "wrong_pin",
                     settings.setupPin.length() == 0 ? "No setup PIN is configured (set it over USB serial)."
                                                     : "Wrong PIN.");
  }
  s->pinFailures_ = 0;
  JsonDocument doc;
  doc["token"] = s->setupToken_;
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleSetupScan(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->setupAuthorized(req)) return ESP_OK;
  int16_t n = WiFi.scanNetworks(false, false);
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int16_t i = 0; i < n && i < 20; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleSetupWifi(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->setupAuthorized(req)) return ESP_OK;
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  String ssid = body["ssid"] | "";
  String password = body["password"] | "";
  if (!s->settings_.setWifi(ssid, password)) {
    return sendError(req, "400 Bad Request", "invalid_wifi", "SSID must be 1-32 characters, password up to 63.");
  }
  s->wifiChanged_ = true;
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Saved. The Hub is connecting to the new network.";
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleSetupAp(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->setupAuthorized(req)) return ESP_OK;
  JsonDocument body;
  if (!readJsonBody(req, body)) {
    return sendError(req, "400 Bad Request", "bad_request", "Expected a JSON body.");
  }
  String password = body["password"] | "";
  if (!s->settings_.setApPassword(password)) {
    return sendError(req, "400 Bad Request", "invalid_password", "Use 8-63 characters, or empty for an open network.");
  }
  s->apChanged_ = true;
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Saved. The student network restarts in a few seconds; reconnect with the new password.";
  return sendJson(req, doc);
}

esp_err_t LocalServer::handleApiNotFound(httpd_req_t *req) {
  return sendError(req, "404 Not Found", "not_found", "Unknown API endpoint.");
}

// ---------------------------------------------------------------------------------- static files

esp_err_t LocalServer::handleStatic(httpd_req_t *req) {
  LocalServer *s = self(req);
  if (!s->allow(req, 1)) return ESP_OK;

  String uri(req->uri);
  int q = uri.indexOf('?');
  if (q >= 0) {
    uri = uri.substring(0, q);
  }
  if (uri == "/" || uri.length() == 0) {
    uri = "/index.html";
  }
  if (uri.indexOf("..") >= 0) {
    return sendError(req, "400 Bad Request", "bad_path", "Invalid path.");
  }

  if (!s->fsMounted_) {
    if (uri == "/index.html") {
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      return httpd_resp_send(req, FALLBACK_PAGE, HTTPD_RESP_USE_STRLEN);
    }
  } else {
    String gz = uri + ".gz";
    bool useGz = LittleFS.exists(gz);
    if (useGz || LittleFS.exists(uri)) {
      File f = LittleFS.open(useGz ? gz : uri, "r");
      if (f) {
        httpd_resp_set_type(req, contentTypeFor(uri));
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (useGz) {
          httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
        uint8_t buf[2048];
        esp_err_t err = ESP_OK;
        while (err == ESP_OK) {
          size_t n = f.read(buf, sizeof(buf));
          if (n == 0) break;
          err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf), n);
        }
        f.close();
        if (err == ESP_OK) {
          err = httpd_resp_send_chunk(req, nullptr, 0);
        }
        return err;
      }
    }
  }

  // Captive portal: Android (/generate_204), iOS (/hotspot-detect.html), Windows (/connecttest.txt)
  // and any other unknown URL are sent to the student page.
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}
