#ifndef ELINGO_BUTTON_MANAGER_H
#define ELINGO_BUTTON_MANAGER_H

#include <Arduino.h>

class ButtonManager {
 public:
  ButtonManager();

  void begin();
  void update();

  bool consumePress();
  bool isPressed() const;

 private:
  int lastReading_;
  int stableState_;
  uint32_t lastDebounceMs_;
  bool pressEvent_;
};

#endif
