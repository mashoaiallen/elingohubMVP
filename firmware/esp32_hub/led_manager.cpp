#include "led_manager.h"

#include "pins.h"

LedManager::LedManager() : override_(-1), pattern_(LedPattern::OFF), patternStartedMs_(0), lastOutput_(0xFF) {}

void LedManager::setOverride(int8_t mask) {
  override_ = mask;
  lastOutput_ = 0xFF;  // force the next write
  update(millis());
}

void LedManager::begin() {
  // Drive all three channels LOW before switching to output so the LED never flashes at boot.
  digitalWrite(Pins::LED_RED, LOW);
  digitalWrite(Pins::LED_GREEN, LOW);
  digitalWrite(Pins::LED_BLUE, LOW);
  pinMode(Pins::LED_RED, OUTPUT);
  pinMode(Pins::LED_GREEN, OUTPUT);
  pinMode(Pins::LED_BLUE, OUTPUT);
  lastOutput_ = 0xFF;
  write(false, false, false);
}

void LedManager::setPattern(LedPattern pattern) {
  if (pattern == pattern_) {
    return;
  }
  Serial.printf("[LED] %s -> %s\n", name(pattern_), name(pattern));
  pattern_ = pattern;
  patternStartedMs_ = millis();
  update(patternStartedMs_);
}

LedPattern LedManager::pattern() const {
  return pattern_;
}

void LedManager::update(uint32_t now) {
  if (override_ >= 0) {
    write(override_ & 1, override_ & 2, override_ & 4);
    return;
  }
  uint32_t t = now - patternStartedMs_;
  switch (pattern_) {
    case LedPattern::OFF:
      write(false, false, false);
      break;
    case LedPattern::BOOT_SWEEP:
      write(t < 300, t >= 300 && t < 600, t >= 600 && t < 900);
      break;
    case LedPattern::OFFLINE:
      write((t % 2000) < 150, false, false);
      break;
    case LedPattern::SETUP: {
      uint32_t p = t % 1500;
      write(false, false, p < 120 || (p >= 240 && p < 360));
      break;
    }
    case LedPattern::SESSION_WAITING:
      write(false, false, (t % 300) < 150);  // blue flicker: 150 ms on, 150 ms off
      break;
    case LedPattern::SESSION_LIVE:
      write(false, true, false);
      break;
    case LedPattern::SESSION_ENDED:
      write(true, false, false);
      break;
    case LedPattern::FATAL:
      write((t % 200) < 100, false, false);
      break;
  }
}

void LedManager::write(bool red, bool green, bool blue) {
  uint8_t output = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0);
  if (output == lastOutput_) {
    return;
  }
  lastOutput_ = output;
  digitalWrite(Pins::LED_RED, red ? HIGH : LOW);
  digitalWrite(Pins::LED_GREEN, green ? HIGH : LOW);
  digitalWrite(Pins::LED_BLUE, blue ? HIGH : LOW);
}

const char *LedManager::name(LedPattern pattern) {
  switch (pattern) {
    case LedPattern::OFF: return "OFF";
    case LedPattern::BOOT_SWEEP: return "BOOT_SWEEP";
    case LedPattern::OFFLINE: return "OFFLINE";
    case LedPattern::SETUP: return "SETUP";
    case LedPattern::SESSION_WAITING: return "SESSION_WAITING(blue flicker)";
    case LedPattern::SESSION_LIVE: return "SESSION_LIVE(green)";
    case LedPattern::SESSION_ENDED: return "SESSION_ENDED(red)";
    case LedPattern::FATAL: return "FATAL";
  }
  return "?";
}
