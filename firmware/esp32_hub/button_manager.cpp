#include "button_manager.h"

#include "config.h"
#include "pins.h"

ButtonManager::ButtonManager()
    : lastReading_(Pins::BUTTON_RELEASED_LEVEL),
      stableState_(Pins::BUTTON_RELEASED_LEVEL),
      lastChangeMs_(0),
      pressStartedMs_(0),
      lastAcceptedShortMs_(0),
      longFired_(false) {}

void ButtonManager::begin() {
  // The internal resistor holds the pin at the released level; the button drives it to the other.
  pinMode(Pins::BUTTON, Pins::BUTTON_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  lastReading_ = digitalRead(Pins::BUTTON);
  stableState_ = lastReading_;
  lastChangeMs_ = millis();
  // A button held during boot must not register as a press.
  longFired_ = stableState_ == Pins::BUTTON_PRESSED_LEVEL;
}

bool ButtonManager::isPressed() const {
  return stableState_ == Pins::BUTTON_PRESSED_LEVEL;
}

ButtonEvent ButtonManager::update(uint32_t now) {
  int reading = digitalRead(Pins::BUTTON);
  if (reading != lastReading_) {
    lastReading_ = reading;
    lastChangeMs_ = now;
  }

  if (now - lastChangeMs_ >= Config::BUTTON_DEBOUNCE_MS && reading != stableState_) {
    stableState_ = reading;
    if (stableState_ == Pins::BUTTON_PRESSED_LEVEL) {
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

  if (stableState_ == Pins::BUTTON_PRESSED_LEVEL && !longFired_ && now - pressStartedMs_ >= Config::BUTTON_LONG_PRESS_MS) {
    longFired_ = true;
    return ButtonEvent::LONG_PRESS;
  }
  return ButtonEvent::NONE;
}
