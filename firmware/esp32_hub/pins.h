#ifndef ELINGO_PINS_H
#define ELINGO_PINS_H

#include <Arduino.h>

// Waveshare ESP32-S3-DEV-KIT-N32R16V. GPIO 26-37 belong to the octal flash/PSRAM and
// GPIO 47/48 run at 1.8 V on this module - do not use them.
namespace Pins {

// Status LED: external common-cathode RGB (or three LEDs sharing ground), each colour through
// its own resistor. HIGH = on.
constexpr uint8_t LED_RED = 11;
constexpr uint8_t LED_GREEN = 12;
constexpr uint8_t LED_BLUE = 13;

// Push button between GPIO10 and GND (internal pull-up, pressed = LOW).
constexpr uint8_t BUTTON = 10;

// INMP441 - verified wiring (see README "Verified microphone configuration").
constexpr uint8_t I2S_SCK = 6;
constexpr uint8_t I2S_WS = 5;
constexpr uint8_t I2S_SD = 4;

}  // namespace Pins

#endif
