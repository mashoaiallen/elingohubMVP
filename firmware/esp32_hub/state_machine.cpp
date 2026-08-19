#include "state_machine.h"

#include "config.h"

StateMachine::StateMachine(
    WifiManager &wifiManager,
    ApiManager &apiManager,
    LcdManager &lcdManager,
    LedManager &ledManager,
    ButtonManager &buttonManager,
    AudioManager &audioManager,
    SessionManager &sessionManager)
    : wifiManager_(wifiManager),
      apiManager_(apiManager),
      lcdManager_(lcdManager),
      ledManager_(ledManager),
      buttonManager_(buttonManager),
      audioManager_(audioManager),
      sessionManager_(sessionManager),
      state_(HubState::BOOTING),
      stateEnteredMs_(0),
      lastHealthAttemptMs_(0),
      lastUploadAttemptMs_(0),
      pendingChunk_() {}

void StateMachine::begin() {
  ledManager_.begin();
  lcdManager_.begin();
  buttonManager_.begin();
  apiManager_.begin();

  wifiManager_.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);

  if (!audioManager_.begin()) {
    Serial.print("Audio init warning: ");
    Serial.println(audioManager_.lastError());
  }

  transitionTo(HubState::BOOTING);
}

void StateMachine::update() {
  uint32_t now = millis();

  buttonManager_.update();
  wifiManager_.update();
  bool buttonPressed = buttonManager_.consumePress();

  switch (state_) {
    case HubState::BOOTING:
      handleBooting(now);
      break;
    case HubState::CONNECTING_WIFI:
      handleConnectingWifi();
      break;
    case HubState::CHECKING_SERVER:
      handleCheckingServer(now);
      break;
    case HubState::ERROR_STATE:
      handleErrorState(now);
      break;
    case HubState::READY:
      handleReady(buttonPressed);
      break;
    case HubState::CREATING_SESSION:
      handleCreatingSession();
      break;
    case HubState::JOINING:
      handleJoining(now);
      break;
    case HubState::RECORDING:
      handleRecording(now, buttonPressed);
      break;
    case HubState::ENDING_SESSION:
      handleEndingSession();
      break;
  }
}

void StateMachine::transitionTo(HubState nextState) {
  state_ = nextState;
  stateEnteredMs_ = millis();

  Serial.print("State: ");
  Serial.println(stateName(state_));

  switch (state_) {
    case HubState::BOOTING:
      ledManager_.setBooting();
      lcdManager_.showBooting();
      break;
    case HubState::CONNECTING_WIFI:
      ledManager_.setBooting();
      lcdManager_.showConnecting();
      break;
    case HubState::CHECKING_SERVER:
      ledManager_.setBooting();
      lcdManager_.showConnecting();
      lastHealthAttemptMs_ = 0;
      break;
    case HubState::ERROR_STATE:
      ledManager_.setError();
      lcdManager_.showServerOffline();
      break;
    case HubState::READY:
      releasePendingChunk();
      audioManager_.stop();
      sessionManager_.reset();
      ledManager_.setReady();
      lcdManager_.showReady();
      break;
    case HubState::CREATING_SESSION:
      ledManager_.setReady();
      lcdManager_.showMessage("Starting", "Session...");
      break;
    case HubState::JOINING:
      ledManager_.setReady();
      lcdManager_.showJoin(sessionManager_.sessionId());
      break;
    case HubState::RECORDING:
      ledManager_.setRecording();
      lcdManager_.showRecording(sessionManager_.sessionId());
      lastUploadAttemptMs_ = 0;
      break;
    case HubState::ENDING_SESSION:
      ledManager_.setBooting();
      lcdManager_.showEnding();
      break;
  }
}

void StateMachine::handleBooting(uint32_t now) {
  if (now - stateEnteredMs_ >= Config::BOOT_SCREEN_MS) {
    transitionTo(HubState::CONNECTING_WIFI);
  }
}

void StateMachine::handleConnectingWifi() {
  if (wifiManager_.isConnected()) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(wifiManager_.localIp());
    transitionTo(HubState::CHECKING_SERVER);
  }
}

void StateMachine::handleCheckingServer(uint32_t now) {
  if (!wifiManager_.isConnected()) {
    transitionTo(HubState::CONNECTING_WIFI);
    return;
  }

  if (lastHealthAttemptMs_ == 0 ||
      now - lastHealthAttemptMs_ >= Config::HEALTH_RETRY_INTERVAL_MS) {
    lastHealthAttemptMs_ = now;
    ApiResult result = apiManager_.checkHealth();
    if (result.success) {
      transitionTo(HubState::READY);
    } else {
      Serial.print("Backend health failed: ");
      Serial.println(result.error);
      transitionTo(HubState::ERROR_STATE);
    }
  }
}

void StateMachine::handleErrorState(uint32_t now) {
  if (!wifiManager_.isConnected()) {
    transitionTo(HubState::CONNECTING_WIFI);
    return;
  }

  if (now - lastHealthAttemptMs_ >= Config::HEALTH_RETRY_INTERVAL_MS) {
    lastHealthAttemptMs_ = now;
    ApiResult result = apiManager_.checkHealth();
    if (result.success) {
      transitionTo(HubState::READY);
    } else {
      Serial.print("Backend still unavailable: ");
      Serial.println(result.error);
      lcdManager_.showServerOffline();
    }
  }
}

void StateMachine::handleReady(bool buttonPressed) {
  if (!wifiManager_.isConnected()) {
    transitionTo(HubState::CONNECTING_WIFI);
    return;
  }

  if (buttonPressed) {
    transitionTo(HubState::CREATING_SESSION);
  }
}

void StateMachine::handleCreatingSession() {
  if (!wifiManager_.isConnected()) {
    transitionTo(HubState::CONNECTING_WIFI);
    return;
  }

  String sessionId;
  ApiResult result = apiManager_.startSession(sessionId);
  if (!result.success) {
    Serial.print("Session creation failed: ");
    Serial.println(result.error);
    transitionTo(HubState::ERROR_STATE);
    return;
  }

  sessionManager_.beginSession(sessionId);
  transitionTo(HubState::JOINING);
}

void StateMachine::handleJoining(uint32_t now) {
  if (!sessionManager_.hasActiveSession()) {
    transitionTo(HubState::READY);
    return;
  }

  if (now - stateEnteredMs_ < Config::JOIN_SCREEN_MS) {
    return;
  }

  if (!audioManager_.start()) {
    Serial.print("Unable to start audio capture: ");
    Serial.println(audioManager_.lastError());

    if (wifiManager_.isConnected()) {
      apiManager_.endSession(sessionManager_.sessionId());
    }
    transitionTo(HubState::READY);
    return;
  }

  transitionTo(HubState::RECORDING);
}

void StateMachine::handleRecording(uint32_t now, bool buttonPressed) {
  if (buttonPressed) {
    audioManager_.stop();
    releasePendingChunk();
    transitionTo(HubState::ENDING_SESSION);
    return;
  }

  if (!sessionManager_.hasActiveSession()) {
    transitionTo(HubState::READY);
    return;
  }

  if (pendingChunk_.valid()) {
    tryUploadPendingChunk(now);
    return;
  }

  if (audioManager_.update()) {
    pendingChunk_ = audioManager_.takeChunk();
    tryUploadPendingChunk(now);
  }
}

void StateMachine::handleEndingSession() {
  if (sessionManager_.hasActiveSession() && wifiManager_.isConnected()) {
    ApiResult result = apiManager_.endSession(sessionManager_.sessionId());
    if (!result.success) {
      Serial.print("Session end request failed: ");
      Serial.println(result.error);
      Serial.println("Session end request failed locally; resetting device state anyway");
    }
  } else if (sessionManager_.hasActiveSession()) {
    Serial.println("WiFi offline while ending session; resetting local state");
  }

  transitionTo(HubState::READY);
}

void StateMachine::tryUploadPendingChunk(uint32_t now) {
  if (!pendingChunk_.valid()) {
    return;
  }

  if (lastUploadAttemptMs_ != 0 &&
      now - lastUploadAttemptMs_ < Config::UPLOAD_RETRY_INTERVAL_MS) {
    return;
  }

  if (!wifiManager_.isConnected()) {
    lastUploadAttemptMs_ = now;
    Serial.println("WiFi unavailable; holding chunk for retry");
    return;
  }

  ApiResult result = apiManager_.uploadAudioChunk(
      sessionManager_.sessionId(),
      sessionManager_.chunkNumber(),
      pendingChunk_.data,
      pendingChunk_.length);

  if (!result.success) {
    lastUploadAttemptMs_ = millis();
    Serial.print("Upload failed for chunk ");
    Serial.print(sessionManager_.chunkNumber());
    Serial.print(": ");
    Serial.println(result.error);
    Serial.println("Retrying later");
    return;
  }

  lastUploadAttemptMs_ = 0;
  audioManager_.releaseChunk(pendingChunk_);
  sessionManager_.advanceChunk();
  lcdManager_.showRecording(sessionManager_.sessionId());

  if (!audioManager_.start()) {
    Serial.print("Unable to restart audio capture: ");
    Serial.println(audioManager_.lastError());
    transitionTo(HubState::ENDING_SESSION);
  }
}

void StateMachine::releasePendingChunk() {
  if (pendingChunk_.valid()) {
    audioManager_.releaseChunk(pendingChunk_);
  }
}

const char *StateMachine::stateName(HubState state) const {
  switch (state) {
    case HubState::BOOTING:
      return "BOOTING";
    case HubState::CONNECTING_WIFI:
      return "CONNECTING_WIFI";
    case HubState::CHECKING_SERVER:
      return "CHECKING_SERVER";
    case HubState::READY:
      return "READY";
    case HubState::CREATING_SESSION:
      return "CREATING_SESSION";
    case HubState::JOINING:
      return "JOINING";
    case HubState::RECORDING:
      return "RECORDING";
    case HubState::ENDING_SESSION:
      return "ENDING_SESSION";
    case HubState::ERROR_STATE:
      return "ERROR_STATE";
    default:
      return "unknown";
  }
}
