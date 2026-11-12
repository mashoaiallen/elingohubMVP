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

// Push button on GPIO9. Moved from GPIO10 on 2026-11-12 (GPIO9 is not a strapping pin, not
// flash/PSRAM, and free since the 16x2 LCD was removed).
// Active-HIGH since firmware 0.3.2: the button connects GPIO9 to 3.3 V (NEVER 5 V) and the
// internal pull-down holds the pin LOW while released. Set BUTTON_ACTIVE_HIGH = false for the
// original wiring (button to GND, internal pull-up, pressed = LOW).
constexpr uint8_t BUTTON = 9;
constexpr bool BUTTON_ACTIVE_HIGH = true;
constexpr int BUTTON_PRESSED_LEVEL = BUTTON_ACTIVE_HIGH ? HIGH : LOW;
constexpr int BUTTON_RELEASED_LEVEL = BUTTON_ACTIVE_HIGH ? LOW : HIGH;

// INMP441 - verified wiring (see README "Verified microphone configuration").
constexpr uint8_t I2S_SCK = 6;
constexpr uint8_t I2S_WS = 5;
constexpr uint8_t I2S_SD = 4;

}  // namespace Pins

#endif
