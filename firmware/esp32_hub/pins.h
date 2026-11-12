#ifndef ELINGO_PINS_H
#define ELINGO_PINS_H

#include <Arduino.h>

// Waveshare ESP32-S3-DEV-KIT-N32R16V. GPIO 26-37 belong to the octal flash/PSRAM and
// GPIO 47/48 run at 1.8 V on this module - do not use them.
namespace Pins {

// Status LED: external common-cathode RGB module (common pin to GND), each colour through its
// own resistor. Active-HIGH: HIGH = on. GPIO 11-13 have no onboard connection on this board (its
// own WS2812 LED is on GPIO38), are not strapping pins and are 3.3 V I/O. Red and green are the
// pins the original firmware already used.
constexpr uint8_t LED_RED = 11;
constexpr uint8_t LED_GREEN = 12;
constexpr uint8_t LED_BLUE = 13;

// Push button between GPIO9 and GND (internal pull-up, pressed = LOW). Moved from GPIO10 on
// 2026-11-12: on the Hub's breadboard the rail-to-GPIO10 link did not carry the press (the pin
// itself worked when the button wire touched it directly). GPIO9 is not a strapping pin, not
// flash/PSRAM, and free since the 16x2 LCD was removed.
constexpr uint8_t BUTTON = 9;

// INMP441 - verified wiring (see README "Verified microphone configuration").
constexpr uint8_t I2S_SCK = 6;
constexpr uint8_t I2S_WS = 5;
constexpr uint8_t I2S_SD = 4;

}  // namespace Pins

#endif
