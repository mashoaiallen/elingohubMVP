#ifndef ELINGO_LED_MANAGER_H
#define ELINGO_LED_MANAGER_H

#include <Arduino.h>

// One RGB status LED (common cathode, HIGH = on). Only one pattern is shown at a time.
//
//  Pattern               LED                                   Meaning
//  OFF                   off                                   Ready / idle (or after a session)
//  BOOT_SWEEP            red, green, blue once (0.9 s)         Power-on self-test
//  OFFLINE               short red blink every 2 s             No upstream Wi-Fi or cloud
//  SETUP                 blue double-blink every 1.5 s         Setup page open (long press)
//  SESSION_WAITING       blue solid (red, green off)           Session started, no translated audio yet
//  SESSION_TRANSLATING   green solid (red, blue off)           First translated audio received by the Hub
//  SESSION_ENDED         red solid (green, blue off)           Session ended; held for 5 s, then OFF
//  FATAL                 red fast blink (5 Hz)                 Hardware fault (microphone/PSRAM)
//
// The three session patterns are always solid: a weak cloud link during a session is reported on
// the student page and the serial log, never by blinking the session colour.
enum class LedPattern : uint8_t {
  OFF,
  BOOT_SWEEP,
  OFFLINE,
  SETUP,
  SESSION_WAITING,
  SESSION_TRANSLATING,
  SESSION_ENDED,
  FATAL
};

class LedManager {
 public:
  LedManager();

  void begin();
  void setPattern(LedPattern pattern);
  void update(uint32_t now);
  LedPattern pattern() const;
  static const char *name(LedPattern pattern);

  // Wiring check from the serial console: force channels (bit 0 red, 1 green, 2 blue) regardless
  // of the hub state. A negative value returns control to the state machine.
  void setOverride(int8_t mask);

 private:
  void write(bool red, bool green, bool blue);

  int8_t override_;
  LedPattern pattern_;
  uint32_t patternStartedMs_;
  uint8_t lastOutput_;
};

#endif
