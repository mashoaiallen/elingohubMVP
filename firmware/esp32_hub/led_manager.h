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
//  SESSION_WAITING       blue solid                            Session started, no translation yet
//  SESSION_TRANSLATING   green solid (red, blue off)           Translated audio is arriving
//  DEGRADED_BLUE/GREEN   that colour blinking 1 Hz             Session running, cloud unreachable
//  SESSION_ENDED         red solid for 5 s, then off           Session ended by button press
//  FATAL                 red fast blink (5 Hz)                 Hardware fault (microphone/PSRAM)
enum class LedPattern : uint8_t {
  OFF,
  BOOT_SWEEP,
  OFFLINE,
  SETUP,
  SESSION_WAITING,
  SESSION_TRANSLATING,
  DEGRADED_BLUE,
  DEGRADED_GREEN,
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

 private:
  void write(bool red, bool green, bool blue);

  LedPattern pattern_;
  uint32_t patternStartedMs_;
  uint8_t lastOutput_;
};

#endif
