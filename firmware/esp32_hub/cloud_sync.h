#ifndef ELINGO_CLOUD_SYNC_H
#define ELINGO_CLOUD_SYNC_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

#include "audio_manager.h"
#include "cloud_client.h"
#include "listener_registry.h"
#include "segment_store.h"
#include "session_manager.h"
#include "settings.h"
#include "wifi_manager.h"

// Outcome of a start request, read by the state machine.
enum class StartState : uint8_t { IDLE, PENDING, STARTED, FAILED };

// The Hub's cloud worker (FreeRTOS task on core 0). Owns all Supabase traffic so network
// latency never blocks the control loop, audio capture or the student web server.
//   * hello/heartbeat, session start/end (idempotent nonce), resume after reboot
//   * upload queued chunks -> Storage, then hub_register_chunk (retries with backoff)
//   * presence: full listener set via hub_sync (on change, at least every 5 s)
//   * hub_poll for new segments of languages with local listeners -> download -> SegmentStore
class CloudSync {
 public:
  CloudSync(SettingsStore &settings, WifiManager &wifi, AudioManager &audio, ListenerRegistry &listeners,
            SegmentStore &segments, SessionManager &session);

  void begin();

  void requestStart();
  StartState startState() const;
  uint32_t startFirstSeq() const;
  void acknowledgeStart();

  void requestEnd();
  bool ending() const;

  // Set after hello when the cloud still has an active session for this Hub (reboot recovery).
  bool takeResume(uint32_t *nextSeq);
  // Set when the cloud ended the session on its own (e.g. timeout).
  bool takeRemoteEnd();

  void reloadSettings();
  bool online() const;
  uint32_t uploadedChunks() const;
  uint32_t lastUploadAgeMs() const;

 private:
  enum class Command : uint8_t { START, END, RELOAD };

  static void taskEntry(void *arg);
  void taskLoop();
  bool doHello();
  void doStart();
  void doEnd();
  bool uploadPending(uint32_t now);
  bool doSync(uint32_t now);
  bool doPoll(uint32_t now);
  void fillStats(JsonObject stats);
  void markOnline(bool ok);
  void backoff(uint32_t now);

  SettingsStore &settings_;
  WifiManager &wifi_;
  AudioManager &audio_;
  ListenerRegistry &listeners_;
  SegmentStore &segments_;
  SessionManager &session_;
  CloudClient client_;

  TaskHandle_t task_;
  QueueHandle_t commands_;

  std::atomic<StartState> startState_;
  std::atomic<uint32_t> startFirstSeq_;
  std::atomic<bool> ending_;
  std::atomic<bool> online_;
  std::atomic<bool> resumeAvailable_;
  std::atomic<uint32_t> resumeSeq_;
  std::atomic<bool> remoteEnd_;
  std::atomic<uint32_t> uploaded_;
  std::atomic<uint32_t> lastUploadOkMs_;

  bool helloDone_;
  uint32_t lastHelloMs_;
  uint32_t nextAttemptMs_;
  uint32_t retryDelayMs_;
  String sessionId_;
  AudioChunk *pending_;
  uint32_t pendingSinceMs_;
  uint32_t nextUploadMs_;
  uint32_t uploadRetryMs_;
  uint32_t lastSyncMs_;
  uint32_t syncedVersion_;
  uint32_t lastPollMs_;
  int64_t lastSegmentId_;
  uint32_t endDeadlineMs_;
};

#endif
