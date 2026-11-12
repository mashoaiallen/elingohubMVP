#include "cloud_sync.h"

#include <esp_random.h>
#include <time.h>

#include <algorithm>

#include "config.h"

namespace {

String randomHex(uint8_t bytes) {
  static const char *HEX_DIGITS = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (uint8_t i = 0; i < bytes; ++i) {
    uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
    out += HEX_DIGITS[b >> 4];
    out += HEX_DIGITS[b & 0x0F];
  }
  return out;
}

String isoTime(int64_t epochMs) {
  time_t secs = static_cast<time_t>(epochMs / 1000);
  struct tm tmv;
  gmtime_r(&secs, &tmv);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(epochMs % 1000));
  return String(buf);
}

String seqName(uint32_t seq) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%06lu", static_cast<unsigned long>(seq));
  return String(buf);
}

}  // namespace

CloudSync::CloudSync(SettingsStore &settings, WifiManager &wifi, AudioManager &audio, ListenerRegistry &listeners,
                     SegmentStore &segments, SessionManager &session)
    : settings_(settings),
      wifi_(wifi),
      audio_(audio),
      listeners_(listeners),
      segments_(segments),
      session_(session),
      task_(nullptr),
      commands_(nullptr),
      startState_(StartState::IDLE),
      startFirstSeq_(1),
      ending_(false),
      online_(false),
      resumeAvailable_(false),
      resumeSeq_(1),
      remoteEnd_(false),
      uploaded_(0),
      lastUploadOkMs_(0),
      helloDone_(false),
      lastHelloMs_(0),
      nextAttemptMs_(0),
      retryDelayMs_(Config::CLOUD_RETRY_MIN_MS),
      pending_(nullptr),
      pendingSinceMs_(0),
      nextUploadMs_(0),
      uploadRetryMs_(Config::CLOUD_RETRY_MIN_MS),
      lastSyncMs_(0),
      syncedVersion_(0xFFFFFFFF),
      lastPollMs_(0),
      lastSegmentId_(0),
      endDeadlineMs_(0) {}

void CloudSync::begin() {
  client_.configure(settings_.snapshot());
  commands_ = xQueueCreate(4, sizeof(Command));
  // TLS needs a generous stack; keep it in internal RAM.
  xTaskCreatePinnedToCore(taskEntry, "cloud", 16384, this, 3, &task_, 0);
}

void CloudSync::requestStart() {
  startState_ = StartState::PENDING;
  Command c = Command::START;
  xQueueSend(commands_, &c, 0);
}

StartState CloudSync::startState() const {
  return startState_;
}

uint32_t CloudSync::startFirstSeq() const {
  return startFirstSeq_;
}

void CloudSync::acknowledgeStart() {
  startState_ = StartState::IDLE;
}

void CloudSync::requestEnd() {
  ending_ = true;
  Command c = Command::END;
  xQueueSend(commands_, &c, 0);
}

bool CloudSync::ending() const {
  return ending_;
}

bool CloudSync::takeResume(uint32_t *nextSeq) {
  if (!resumeAvailable_.exchange(false)) {
    return false;
  }
  *nextSeq = resumeSeq_;
  return true;
}

bool CloudSync::takeRemoteEnd() {
  return remoteEnd_.exchange(false);
}

void CloudSync::reloadSettings() {
  Command c = Command::RELOAD;
  xQueueSend(commands_, &c, 0);
}

bool CloudSync::online() const {
  return online_;
}

uint32_t CloudSync::uploadedChunks() const {
  return uploaded_;
}

uint32_t CloudSync::lastUploadAgeMs() const {
  uint32_t last = lastUploadOkMs_;
  return last == 0 ? UINT32_MAX : millis() - last;
}

void CloudSync::taskEntry(void *arg) {
  static_cast<CloudSync *>(arg)->taskLoop();
}

void CloudSync::markOnline(bool ok) {
  if (ok) {
    retryDelayMs_ = Config::CLOUD_RETRY_MIN_MS;
  }
  if (online_.exchange(ok) != ok) {
    Serial.printf("[Cloud] %s\n", ok ? "online" : "OFFLINE");
  }
  session_.setCloudOnline(ok);
}

void CloudSync::backoff(uint32_t now) {
  nextAttemptMs_ = now + retryDelayMs_;
  retryDelayMs_ = std::min<uint32_t>(retryDelayMs_ * 2, Config::CLOUD_RETRY_MAX_MS);
}

void CloudSync::fillStats(JsonObject stats) {
  stats["fw"] = Config::FIRMWARE_VERSION;
  stats["uptimeS"] = millis() / 1000;
  stats["heap"] = ESP.getFreeHeap();
  stats["minHeap"] = ESP.getMinFreeHeap();
  stats["psramFree"] = ESP.getFreePsram();
  stats["rssi"] = wifi_.staRssi();
  stats["channel"] = wifi_.channel();
  stats["stations"] = wifi_.stationCount();
  stats["listeners"] = listeners_.count();
  stats["queued"] = audio_.queuedChunks();
  stats["dropped"] = audio_.droppedChunks();
  stats["segments"] = segments_.segmentCount();
  stats["segmentBytes"] = segments_.bytesUsed();
}

void CloudSync::taskLoop() {
  for (;;) {
    uint32_t now = millis();

    Command cmd;
    while (xQueueReceive(commands_, &cmd, 0) == pdTRUE) {
      if (cmd == Command::RELOAD) {
        client_.configure(settings_.snapshot());
        helloDone_ = false;
      } else if (cmd == Command::START) {
        doStart();
      } else if (cmd == Command::END) {
        endDeadlineMs_ = millis() + Config::END_FLUSH_MAX_MS;
      }
    }

    if (!wifi_.staConnected() || !client_.configured()) {
      markOnline(false);
      if (startState_ == StartState::PENDING) {
        startState_ = StartState::FAILED;
      }
      if (ending_) {
        doEnd();  // cannot reach the cloud: finish locally; the cloud times the session out
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (static_cast<int32_t>(now - nextAttemptMs_) < 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!helloDone_ || (sessionId_.length() == 0 && now - lastHelloMs_ >= Config::IDLE_HELLO_MS)) {
      if (!doHello()) {
        backoff(now);
        continue;
      }
    }

    if (sessionId_.length() > 0) {
      bool worked = uploadPending(now);
      if (!ending_) {
        doSync(now);
        doPoll(now);
      } else if (pending_ == nullptr && audio_.queuedChunks() == 0) {
        doEnd();  // everything recorded before the press has been uploaded
      } else if (static_cast<int32_t>(now - endDeadlineMs_) >= 0) {
        Serial.println("[Cloud] End flush timed out; remaining chunks discarded");
        doEnd();
      }
      vTaskDelay(pdMS_TO_TICKS(worked ? 1 : 20));
    } else {
      if (ending_) {
        ending_ = false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

bool CloudSync::doHello() {
  JsonDocument args;
  args["p_firmware_version"] = Config::FIRMWARE_VERSION;
  fillStats(args["p_stats"].to<JsonObject>());
  JsonDocument resp;
  CloudResult r = client_.rpc("hub_hello", args, resp);
  if (!r.ok) {
    Serial.printf("[Cloud] hello failed: %s\n", r.error.c_str());
    markOnline(false);
    return false;
  }
  markOnline(true);
  lastHelloMs_ = millis();
  session_.setHub(resp["hub"]["id"] | "", resp["hub"]["name"] | "");
  session_.setCatalog(resp["languages"].as<JsonArrayConst>());
  if (!helloDone_) {
    Serial.printf("[Cloud] Hub \"%s\" ready, %u languages\n", resp["hub"]["name"] | "?",
                  static_cast<unsigned>(resp["languages"].size()));
    JsonVariantConst s = resp["session"];
    if (!s.isNull() && sessionId_.length() == 0 && startState_ == StartState::IDLE) {
      sessionId_ = s["id"] | "";
      resumeSeq_ = s["nextSeq"] | 1;
      lastSegmentId_ = 0;
      session_.beginSession(sessionId_);
      resumeAvailable_ = true;
      Serial.printf("[Cloud] Resuming session %s at seq %lu\n", sessionId_.c_str(),
                    static_cast<unsigned long>(resumeSeq_.load()));
    }
  }
  helloDone_ = true;
  return true;
}

void CloudSync::doStart() {
  if (!wifi_.staConnected() || !client_.configured()) {
    startState_ = StartState::FAILED;
    return;
  }
  // The same nonce is reused for every retry of this press, so a lost response cannot create
  // two sessions.
  String nonce = randomHex(8);
  for (int attempt = 0; attempt < 3; ++attempt) {
    JsonDocument args;
    args["p_client_nonce"] = nonce;
    JsonDocument resp;
    CloudResult r = client_.rpc("hub_start_session", args, resp);
    if (r.ok) {
      markOnline(true);
      sessionId_ = resp["id"] | "";
      startFirstSeq_ = resp["nextSeq"] | 1;
      lastSegmentId_ = 0;
      syncedVersion_ = 0xFFFFFFFF;
      lastSyncMs_ = 0;
      lastPollMs_ = 0;
      lastUploadOkMs_ = millis();
      segments_.clear();
      remoteEnd_ = false;  // never carry an end signal from a previous session into this one
      session_.beginSession(sessionId_);
      startState_ = StartState::STARTED;
      Serial.printf("[Cloud] Session %s started\n", sessionId_.c_str());
      return;
    }
    Serial.printf("[Cloud] start_session failed (attempt %d): %s\n", attempt + 1, r.error.c_str());
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  markOnline(false);
  startState_ = StartState::FAILED;
}

void CloudSync::doEnd() {
  if (sessionId_.length() > 0 && wifi_.staConnected() && client_.configured()) {
    JsonDocument args;
    args["p_session_id"] = sessionId_;
    JsonDocument resp;
    CloudResult r = client_.rpc("hub_end_session", args, resp);
    Serial.printf("[Cloud] Session %s end: %s\n", sessionId_.c_str(), r.ok ? "ok" : r.error.c_str());
  }
  if (pending_ != nullptr) {
    audio_.releaseChunk(pending_);
    pending_ = nullptr;
  }
  audio_.discardQueued();
  sessionId_ = "";
  segments_.clear();
  session_.endSession();
  remoteEnd_ = false;
  ending_ = false;
}

bool CloudSync::uploadPending(uint32_t now) {
  if (pending_ == nullptr) {
    pending_ = audio_.takeChunk(0);
    if (pending_ == nullptr) {
      bool stale = lastUploadOkMs_ != 0 && now - lastUploadOkMs_ > Config::UPLOAD_DEGRADED_AFTER_MS + Config::AUDIO_CHUNK_MS;
      session_.setDegraded(stale && audio_.isCapturing());
      return false;
    }
    pendingSinceMs_ = now;
    uploadRetryMs_ = Config::CLOUD_RETRY_MIN_MS;
    nextUploadMs_ = now;
  }
  if (static_cast<int32_t>(now - nextUploadMs_) < 0) {
    return false;
  }

  String hubId = session_.hubId();
  String path = hubId + "/" + sessionId_ + "/" + seqName(pending_->seq) + ".wav";
  uint32_t t0 = millis();
  CloudResult up = client_.upload("source-audio", path, pending_->data, pending_->length, "audio/wav");
  bool registered = false;
  CloudResult reg;
  if (up.ok) {
    JsonDocument args;
    args["p_session_id"] = sessionId_;
    args["p_seq"] = pending_->seq;
    args["p_duration_ms"] = pending_->durationMs;
    args["p_rms"] = pending_->rms;
    args["p_peak"] = pending_->peak;
    args["p_silent"] = pending_->silent;
    if (pending_->capturedEpochMs > 0) {
      args["p_captured_at"] = isoTime(pending_->capturedEpochMs);
    }
    JsonDocument resp;
    reg = client_.rpc("hub_register_chunk", args, resp);
    registered = reg.ok;
  }

  if (registered) {
    Serial.printf("[Cloud] Chunk %lu uploaded+registered in %lu ms (%u queued)\n",
                  static_cast<unsigned long>(pending_->seq), static_cast<unsigned long>(millis() - t0),
                  static_cast<unsigned>(audio_.queuedChunks()));
    audio_.releaseChunk(pending_);
    pending_ = nullptr;
    uploaded_++;
    lastUploadOkMs_ = millis();
    session_.setDegraded(false);
    markOnline(true);
    return true;
  }

  const CloudResult &failure = up.ok ? reg : up;
  Serial.printf("[Cloud] Chunk %lu %s failed: %s\n", static_cast<unsigned long>(pending_->seq),
                up.ok ? "register" : "upload", failure.error.c_str());
  if (failure.pgCode == "P0001") {
    // The cloud says the session is over: stop.
    audio_.releaseChunk(pending_);
    pending_ = nullptr;
    remoteEnd_ = true;
    return false;
  }
  if (failure.httpCode == 413 || failure.pgCode == "22023") {
    audio_.releaseChunk(pending_);  // will never succeed
    pending_ = nullptr;
    return false;
  }
  nextUploadMs_ = now + uploadRetryMs_;
  uploadRetryMs_ = std::min<uint32_t>(uploadRetryMs_ * 2, 10000);
  if (now - pendingSinceMs_ > Config::UPLOAD_DEGRADED_AFTER_MS) {
    session_.setDegraded(true);
    markOnline(false);
  }
  return false;
}

bool CloudSync::doSync(uint32_t now) {
  uint32_t version = listeners_.version();
  bool changed = version != syncedVersion_;
  if (!(changed && now - lastSyncMs_ >= Config::LISTENER_SYNC_MIN_GAP_MS) && now - lastSyncMs_ < Config::LISTENER_SYNC_MS) {
    return false;
  }
  lastSyncMs_ = now;
  JsonDocument args;
  args["p_session_id"] = sessionId_;
  listeners_.toJson(args["p_listeners"].to<JsonArray>());
  fillStats(args["p_stats"].to<JsonObject>());
  JsonDocument resp;
  CloudResult r = client_.rpc("hub_sync", args, resp);
  if (!r.ok) {
    Serial.printf("[Cloud] sync failed: %s\n", r.error.c_str());
    if (r.pgCode == "P0002") {
      remoteEnd_ = true;
    }
    return false;
  }
  markOnline(true);
  syncedVersion_ = version;
  session_.setCloudCounts(resp["languages"].as<JsonArrayConst>());
  const char *status = resp["sessionStatus"] | "active";
  if (strcmp(status, "active") != 0) {
    Serial.println("[Cloud] Session ended by the cloud");
    remoteEnd_ = true;
  }
  return true;
}

bool CloudSync::doPoll(uint32_t now) {
  if (now - lastPollMs_ < Config::CLOUD_POLL_MS) {
    return false;
  }
  lastPollMs_ = now;
  char langs[Config::MAX_LANGUAGES][8];
  uint8_t langCount = listeners_.activeLanguages(langs, Config::MAX_LANGUAGES);
  if (langCount == 0) {
    return false;  // nobody listening on this Hub: nothing to fetch
  }

  JsonDocument args;
  args["p_session_id"] = sessionId_;
  args["p_after_segment_id"] = lastSegmentId_;
  JsonArray arr = args["p_languages"].to<JsonArray>();
  for (uint8_t i = 0; i < langCount; ++i) {
    arr.add(langs[i]);
  }
  JsonDocument resp;
  CloudResult r = client_.rpc("hub_poll", args, resp);
  if (!r.ok) {
    Serial.printf("[Cloud] poll failed: %s\n", r.error.c_str());
    return false;
  }
  markOnline(true);
  session_.setCloudCounts(resp["languages"].as<JsonArrayConst>());

  for (JsonObjectConst seg : resp["segments"].as<JsonArrayConst>()) {
    int64_t id = seg["id"] | 0LL;
    const char *lang = seg["lang"] | "";
    uint32_t seq = seg["seq"] | 0;
    const char *path = seg["path"] | "";
    uint32_t durationMs = seg["durationMs"] | 0;
    uint8_t *data = nullptr;
    size_t length = 0;
    uint32_t t0 = millis();
    CloudResult d = client_.download("translated-audio", path, Config::MAX_SEGMENT_BYTES, &data, &length);
    if (!d.ok) {
      Serial.printf("[Cloud] download %s/%lu failed: %s\n", lang, static_cast<unsigned long>(seq), d.error.c_str());
      return false;  // keep the cursor; retry on the next poll
    }
    segments_.put(lang, id, seq, durationMs, data, length);
    lastSegmentId_ = std::max<int64_t>(lastSegmentId_, id);
    session_.setTranslating(true);
    Serial.printf("[Cloud] Segment %s/%lu (%u bytes) in %lu ms\n", lang, static_cast<unsigned long>(seq),
                  static_cast<unsigned>(length), static_cast<unsigned long>(millis() - t0));
  }
  return true;
}
