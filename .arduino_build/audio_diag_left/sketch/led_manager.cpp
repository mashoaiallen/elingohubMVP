#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\led_manager.cpp"
#include "led_manager.h"

#include "pins.h"

void LedManager::begin() {
  pinMode(Pins::RED_LED, OUTPUT);
  pinMode(Pins::GREEN_LED, OUTPUT);
  setBooting();
}

void LedManager::setBooting() {
  apply(true, false);
}

void LedManager::setReady() {
  apply(true, false);
}

void LedManager::setRecording() {
  apply(false, true);
}

void LedManager::setError() {
  apply(true, false);
}

void LedManager::apply(bool redOn, bool greenOn) {
  digitalWrite(Pins::RED_LED, redOn ? HIGH : LOW);
  digitalWrite(Pins::GREEN_LED, greenOn ? HIGH : LOW);
}
