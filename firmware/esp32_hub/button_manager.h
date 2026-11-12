#ifndef ELINGO_BUTTON_MANAGER_H
#define ELINGO_BUTTON_MANAGER_H

#include <Arduino.h>

// Short press (released before 5 s): start / stop the lecture session. A second short press
// within 2 s of the last accepted one is ignored, so bounce or double taps cannot start and
// immediately stop a session.
// Long press (held 5 s): toggle setup mode (Wi-Fi provisioning page). Fires while still held,
// never deletes saved configuration.
enum class ButtonEvent : uint8_t { NONE, SHORT_PRESS, LONG_PRESS };

class ButtonManager {
 public:
  ButtonManager();

  void begin();
  ButtonEvent update(uint32_t now);
  bool isPressed() const;

 private:
  int lastReading_;
  int stableState_;
  uint32_t lastChangeMs_;
  uint32_t pressStartedMs_;
  uint32_t lastAcceptedShortMs_;
  bool longFired_;
};

#endif
