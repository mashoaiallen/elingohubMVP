#ifndef ELINGO_STATE_MACHINE_H
#define ELINGO_STATE_MACHINE_H

#include <Arduino.h>

#include "audio_manager.h"
#include "button_manager.h"
#include "cloud_sync.h"
#include "led_manager.h"
#include "listener_registry.h"
#include "local_server.h"
#include "segment_store.h"
#include "session_manager.h"
#include "settings.h"
#include "wifi_manager.h"

// Hub control loop (Arduino loop task). Network work happens in CloudSync and LocalServer; this
// class only reacts to the button, task results and timers, and drives the status LED.
//
//   BOOTING --> OFFLINE <--> READY --press--> STARTING --cloud ok--> IN_SESSION --press--> SESSION_ENDED
//                  ^         LED off               blue        blue, then green (solid)    red (solid)
//                  |                              |  failed/timeout        |  cloud ended it      | 5 s
//                  +------------------------------+-----------------------+----------------------+
//   STARTING stays blue and waits until the previous session has finished ending before asking
//   the cloud. Presses in STARTING and SESSION_ENDED are ignored. Any state --> FATAL on a
//   microphone/PSRAM fault. Long press (5 s) toggles setup mode anywhere.
class StateMachine {
 public:
  StateMachine(SettingsStore &settings, WifiManager &wifi, LedManager &led, ButtonManager &button,
               AudioManager &audio, SessionManager &session, ListenerRegistry &listeners, SegmentStore &segments,
               CloudSync &cloud, LocalServer &server);

  void begin(bool hardwareFault);
  void update();
  const char *stateName() const;
  void toggleSetupMode();

 private:
  enum class HubState : uint8_t { BOOTING, OFFLINE, READY, STARTING, IN_SESSION, SESSION_ENDED, FATAL };

  void transitionTo(HubState next);
  void handleButton(ButtonEvent event);
  void beginCapture(uint32_t firstSeq);
  void endSession(const char *reason);
  void applySetupChanges(uint32_t now);
  void refreshLed();

  SettingsStore &settings_;
  WifiManager &wifi_;
  LedManager &led_;
  ButtonManager &button_;
  AudioManager &audio_;
  SessionManager &session_;
  ListenerRegistry &listeners_;
  SegmentStore &segments_;
  CloudSync &cloud_;
  LocalServer &server_;

  HubState state_;
  uint32_t stateEnteredMs_;
  uint32_t lastExpiryMs_;
  uint32_t apRestartAtMs_;
  uint32_t startRequestedMs_;
  bool startRequested_;
  bool hardwareFault_;
  bool listenerJoined_;  // a student has joined during this session (LED green)
};

#endif
