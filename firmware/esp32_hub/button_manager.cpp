#include "button_manager.h"

#include "config.h"
#include "pins.h"

ButtonManager::ButtonManager()
    : lastReading_(HIGH),
      stableState_(HIGH),
      lastChangeMs_(0),
      pressStartedMs_(0),
      lastAcceptedShortMs_(0),
      longFired_(false) {}

void ButtonManager::begin() {
  pinMode(Pins::BUTTON, INPUT_PULLUP);
  lastReading_ = digitalRead(Pins::BUTTON);
  stableState_ = lastReading_;
  lastChangeMs_ = millis();
  // A button held during boot must not register as a press.
  longFired_ = stableState_ == LOW;
}

bool ButtonManager::isPressed() const {
  return stableState_ == LOW;
}

ButtonEvent ButtonManager::update(uint32_t now) {
  int reading = digitalRead(Pins::BUTTON);
  if (reading != lastReading_) {
    lastReading_ = reading;
    lastChangeMs_ = now;
  }

  if (now - lastChangeMs_ >= Config::BUTTON_DEBOUNCE_MS && reading != stableState_) {
    stableState_ = reading;
    if (stableState_ == LOW) {
      pressStartedMs_ = now;
      longFired_ = false;
    } else {
      // Released.
      bool wasLong = longFired_;
      longFired_ = false;
      if (!wasLong) {
        if (lastAcceptedShortMs_ != 0 && now - lastAcceptedShortMs_ < Config::BUTTON_LOCKOUT_MS) {
          Serial.println("[Button] Press ignored (lockout)");
          return ButtonEvent::NONE;
        }
        lastAcceptedShortMs_ = now;
        return ButtonEvent::SHORT_PRESS;
      }
    }
  }

  if (stableState_ == LOW && !longFired_ && now - pressStartedMs_ >= Config::BUTTON_LONG_PRESS_MS) {
    longFired_ = true;
    return ButtonEvent::LONG_PRESS;
  }
  return ButtonEvent::NONE;
}
