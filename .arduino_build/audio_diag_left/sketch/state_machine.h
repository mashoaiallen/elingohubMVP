#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\state_machine.h"
#ifndef ELINGO_STATE_MACHINE_H
#define ELINGO_STATE_MACHINE_H

#include <Arduino.h>

#include "api_manager.h"
#include "audio_manager.h"
#include "button_manager.h"
#include "lcd_manager.h"
#include "led_manager.h"
#include "session_manager.h"
#include "wifi_manager.h"

class StateMachine {
 public:
  StateMachine(
      WifiManager &wifiManager,
      ApiManager &apiManager,
      LcdManager &lcdManager,
      LedManager &ledManager,
      ButtonManager &buttonManager,
      AudioManager &audioManager,
      SessionManager &sessionManager);

  void begin();
  void update();

 private:
  enum class HubState {
    BOOTING,
    CONNECTING_WIFI,
    CHECKING_SERVER,
    READY,
    CREATING_SESSION,
    JOINING,
    RECORDING,
    ENDING_SESSION,
    ERROR_STATE
  };

  void transitionTo(HubState nextState);
  void handleBooting(uint32_t now);
  void handleConnectingWifi();
  void handleCheckingServer(uint32_t now);
  void handleErrorState(uint32_t now);
  void handleReady(bool buttonPressed);
  void handleCreatingSession();
  void handleJoining(uint32_t now);
  void handleRecording(uint32_t now, bool buttonPressed);
  void handleEndingSession();
  void tryUploadPendingChunk(uint32_t now);
  void releasePendingChunk();
  const char *stateName(HubState state) const;

  WifiManager &wifiManager_;
  ApiManager &apiManager_;
  LcdManager &lcdManager_;
  LedManager &ledManager_;
  ButtonManager &buttonManager_;
  AudioManager &audioManager_;
  SessionManager &sessionManager_;

  HubState state_;
  uint32_t stateEnteredMs_;
  uint32_t lastHealthAttemptMs_;
  uint32_t lastUploadAttemptMs_;
  AudioChunk pendingChunk_;
};

#endif
