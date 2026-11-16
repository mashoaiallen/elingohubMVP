#include "state_machine.h"

#include "config.h"

StateMachine::StateMachine(SettingsStore &settings, WifiManager &wifi, LedManager &led, ButtonManager &button,
                           AudioManager &audio, SessionManager &session, ListenerRegistry &listeners,
                           SegmentStore &segments, CloudSync &cloud, LocalServer &server)
    : settings_(settings),
      wifi_(wifi),
      led_(led),
      button_(button),
      audio_(audio),
      session_(session),
      listeners_(listeners),
      segments_(segments),
      cloud_(cloud),
      server_(server),
      state_(HubState::BOOTING),
      stateEnteredMs_(0),
      lastExpiryMs_(0),
      apRestartAtMs_(0),
      startRequestedMs_(0),
      startRequested_(false),
      hardwareFault_(false),
      listenerJoined_(false) {}

void StateMachine::begin(bool hardwareFault) {
  hardwareFault_ = hardwareFault;
  transitionTo(HubState::BOOTING);
}

const char *StateMachine::stateName() const {
  switch (state_) {
    case HubState::BOOTING: return "BOOTING";
    case HubState::OFFLINE: return "OFFLINE";
    case HubState::READY: return "READY";
    case HubState::STARTING: return "STARTING";
    case HubState::IN_SESSION: return "IN_SESSION";
    case HubState::SESSION_ENDED: return "SESSION_ENDED";
    case HubState::FATAL: return "FATAL";
  }
  return "?";
}

void StateMachine::transitionTo(HubState next) {
  state_ = next;
  stateEnteredMs_ = millis();
  Serial.printf("[Hub] State: %s\n", stateName());
  refreshLed();
}

void StateMachine::toggleSetupMode() {
  if (server_.setupActive()) {
    server_.disableSetup();
  } else {
    server_.enableSetup(Config::SETUP_MODE_MS);
  }
}

void StateMachine::beginCapture(uint32_t firstSeq) {
  if (!audio_.startCapture(firstSeq)) {
    Serial.printf("[Hub] Cannot start capture: %s\n", audio_.lastError());
    cloud_.requestEnd();
    transitionTo(HubState::FATAL);
    return;
  }
  listenerJoined_ = false;
  transitionTo(HubState::IN_SESSION);
}

// Called after the LED has already changed, so the colour follows the event immediately even
// though stopping the microphone can take up to a second.
void StateMachine::endSession(const char *reason) {
  Serial.printf("[Hub] Ending session (%s)\n", reason);
  audio_.stopCapture();  // flushes the last partial chunk into the upload queue
  cloud_.requestEnd();   // uploads what is left (<= 10 s), ends the session in the cloud, clears
                         // the Hub's audio and tells phones the lecture has ended
}

void StateMachine::handleButton(ButtonEvent event) {
  if (event == ButtonEvent::LONG_PRESS) {
    toggleSetupMode();
    return;
  }
  if (event != ButtonEvent::SHORT_PRESS) {
    return;
  }
  switch (state_) {
    case HubState::READY:
      Serial.println("[Hub] Button: start session");
      startRequested_ = false;           // sent from update() once any previous session has fully ended
      transitionTo(HubState::STARTING);  // blue on now
      break;
    case HubState::IN_SESSION:
      Serial.println("[Hub] Button: end session");
      transitionTo(HubState::SESSION_ENDED);  // red on now, held for 10 s
      endSession("button");
      break;
    case HubState::STARTING:
    case HubState::SESSION_ENDED:
      // Repeated presses while a start or an end is in progress are ignored on purpose.
      Serial.printf("[Hub] Button ignored in %s\n", stateName());
      break;
    case HubState::OFFLINE:
      Serial.println("[Hub] Button ignored: no cloud connection (red blink = offline)");
      break;
    default:
      Serial.printf("[Hub] Button ignored in %s\n", stateName());
      break;
  }
}

void StateMachine::applySetupChanges(uint32_t now) {
  if (server_.takeWifiChanged()) {
    HubSettings s = settings_.snapshot();
    wifi_.setStaCredentials(s.staSsid, s.staPassword);
  }
  if (server_.takeApChanged()) {
    apRestartAtMs_ = now + 3000;  // let the HTTP response reach the phone first
  }
  if (apRestartAtMs_ != 0 && static_cast<int32_t>(now - apRestartAtMs_) >= 0) {
    apRestartAtMs_ = 0;
    wifi_.restartAp(settings_.snapshot().apPassword);
  }
}

void StateMachine::update() {
  uint32_t now = millis();
  handleButton(button_.update(now));
  server_.update(now);
  applySetupChanges(now);

  if (now - lastExpiryMs_ >= 1000) {
    lastExpiryMs_ = now;
    listeners_.expire(now);
  }

  uint32_t resumeSeq = 0;
  switch (state_) {
    case HubState::BOOTING:
      if (now - stateEnteredMs_ >= Config::BOOT_SWEEP_MS) {
        transitionTo(hardwareFault_ ? HubState::FATAL : HubState::OFFLINE);
      }
      break;

    case HubState::OFFLINE:
    case HubState::READY:
      if (cloud_.takeResume(&resumeSeq)) {
        Serial.println("[Hub] Resuming the session that was running before the restart");
        beginCapture(resumeSeq);
      } else if (state_ == HubState::OFFLINE && cloud_.online()) {
        transitionTo(HubState::READY);
      } else if (state_ == HubState::READY && !cloud_.online()) {
        transitionTo(HubState::OFFLINE);
      }
      break;

    case HubState::STARTING: {
      if (!startRequested_) {
        if (!cloud_.ending()) {
          cloud_.requestStart();
          startRequested_ = true;
          startRequestedMs_ = now;
        } else if (now - stateEnteredMs_ > Config::END_FLUSH_MAX_MS + Config::SESSION_START_TIMEOUT_MS) {
          Serial.println("[Hub] Session start failed: previous session still ending");
          transitionTo(cloud_.online() ? HubState::READY : HubState::OFFLINE);
        }
        break;  // previous session still uploading its last audio: stay blue and wait
      }
      StartState start = cloud_.startState();
      if (start == StartState::STARTED) {
        cloud_.acknowledgeStart();
        beginCapture(cloud_.startFirstSeq());
      } else if (start == StartState::FAILED) {
        cloud_.acknowledgeStart();
        Serial.println("[Hub] Session start failed");
        transitionTo(cloud_.online() ? HubState::READY : HubState::OFFLINE);
      } else if (now - startRequestedMs_ > Config::SESSION_START_TIMEOUT_MS) {
        cloud_.acknowledgeStart();
        // The request may still succeed later; ending it means no session is left running in the
        // cloud while the Hub is not recording.
        cloud_.requestEnd();
        Serial.println("[Hub] Session start timed out");
        transitionTo(cloud_.online() ? HubState::READY : HubState::OFFLINE);
      }
      break;
    }

    case HubState::IN_SESSION:
      if (audio_.hardwareFault()) {
        Serial.printf("[Hub] Microphone fault: %s\n", audio_.lastError());
        transitionTo(HubState::FATAL);
        endSession("hardware fault");
      } else if (cloud_.takeRemoteEnd()) {
        transitionTo(HubState::SESSION_ENDED);
        endSession("ended by cloud");
      }
      break;

    case HubState::SESSION_ENDED:
      if (now - stateEnteredMs_ >= Config::SESSION_END_RED_MS) {
        transitionTo(cloud_.online() ? HubState::READY : HubState::OFFLINE);
      }
      break;

    case HubState::FATAL:
      break;
  }

  if (state_ == HubState::IN_SESSION && !listenerJoined_ && listeners_.count() > 0) {
    listenerJoined_ = true;  // LED: blue flicker -> green
    Serial.println("[Hub] First student joined: LED green");
  }
  refreshLed();
  led_.update(now);
}

void StateMachine::refreshLed() {
  LedPattern pattern = LedPattern::OFF;
  switch (state_) {
    case HubState::BOOTING:
      pattern = LedPattern::BOOT_SWEEP;
      break;
    case HubState::FATAL:
      pattern = LedPattern::FATAL;
      break;
    case HubState::SESSION_ENDED:
      pattern = LedPattern::SESSION_ENDED;  // red for 10 s, then off
      break;
    case HubState::STARTING:
      pattern = LedPattern::SESSION_WAITING;  // blue flicker as soon as the button is pressed
      break;
    case HubState::IN_SESSION:
      // Blue flicker until the first student joins (chooses a language on the Hub page), then solid
      // green for the rest of the session, even if students leave. A weak cloud link never
      // changes the colour (see the student page).
      pattern = listenerJoined_ ? LedPattern::SESSION_LIVE : LedPattern::SESSION_WAITING;
      break;
    case HubState::OFFLINE:
      pattern = server_.setupActive() ? LedPattern::SETUP : LedPattern::OFFLINE;
      break;
    case HubState::READY:
      pattern = server_.setupActive() ? LedPattern::SETUP : LedPattern::OFF;
      break;
  }
  led_.setPattern(pattern);
}
