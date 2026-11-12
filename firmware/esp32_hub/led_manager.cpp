#include "led_manager.h"

#include "pins.h"

LedManager::LedManager() : pattern_(LedPattern::OFF), patternStartedMs_(0), lastOutput_(0xFF) {}

void LedManager::begin() {
  pinMode(Pins::LED_RED, OUTPUT);
  pinMode(Pins::LED_GREEN, OUTPUT);
  pinMode(Pins::LED_BLUE, OUTPUT);
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
      write(false, false, true);
      break;
    case LedPattern::SESSION_TRANSLATING:
      write(false, true, false);
      break;
    case LedPattern::DEGRADED_BLUE:
      write(false, false, (t % 1000) < 500);
      break;
    case LedPattern::DEGRADED_GREEN:
      write(false, (t % 1000) < 500, false);
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
    case LedPattern::SESSION_WAITING: return "SESSION_WAITING(blue)";
    case LedPattern::SESSION_TRANSLATING: return "SESSION_TRANSLATING(green)";
    case LedPattern::DEGRADED_BLUE: return "DEGRADED(blue blink)";
    case LedPattern::DEGRADED_GREEN: return "DEGRADED(green blink)";
    case LedPattern::SESSION_ENDED: return "SESSION_ENDED(red)";
    case LedPattern::FATAL: return "FATAL";
  }
  return "?";
}
