#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\led_manager.h"
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
