#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\button_manager.cpp"
#include "button_manager.h"

#include "config.h"
#include "pins.h"

ButtonManager::ButtonManager()
    : lastReading_(HIGH),
      stableState_(HIGH),
      lastDebounceMs_(0),
      pressEvent_(false) {}

void ButtonManager::begin() {
  pinMode(Pins::BUTTON, INPUT_PULLUP);
  lastReading_ = digitalRead(Pins::BUTTON);
  stableState_ = lastReading_;
  lastDebounceMs_ = millis();
  pressEvent_ = false;
}

void ButtonManager::update() {
  int reading = digitalRead(Pins::BUTTON);
  uint32_t now = millis();

  if (reading != lastReading_) {
    lastDebounceMs_ = now;
    lastReading_ = reading;
  }

  if (now - lastDebounceMs_ >= Config::BUTTON_DEBOUNCE_MS && reading != stableState_) {
    stableState_ = reading;
    if (stableState_ == LOW) {
      pressEvent_ = true;
    }
  }
}

bool ButtonManager::consumePress() {
  if (!pressEvent_) {
    return false;
  }

  pressEvent_ = false;
  return true;
}

bool ButtonManager::isPressed() const {
  return stableState_ == LOW;
}
