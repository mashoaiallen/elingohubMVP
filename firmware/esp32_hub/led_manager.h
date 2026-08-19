#ifndef ELINGO_LED_MANAGER_H
#define ELINGO_LED_MANAGER_H

#include <Arduino.h>

class LedManager {
 public:
  void begin();

  void setBooting();
  void setReady();
  void setRecording();
  void setError();

 private:
  void apply(bool redOn, bool greenOn);
};

#endif
