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

// Push button on GPIO2 (firmware 0.3.3). History: GPIO10, then GPIO9 - both worked only with
// the wire touching the pin itself, not through the breadboard, so the button moved to a pin on
// the board's other header. On the ESP32-S3, GPIO2 is not a strapping pin (only 0, 3, 45, 46
// are) and nothing on this board uses it.
// Active-HIGH: the button connects GPIO2 to 3.3 V (NEVER 5 V) and the internal pull-down holds
// the pin LOW while released. Set BUTTON_ACTIVE_HIGH = false for button-to-GND wiring
// (internal pull-up, pressed = LOW).
constexpr uint8_t BUTTON = 2;
constexpr bool BUTTON_ACTIVE_HIGH = true;
constexpr int BUTTON_PRESSED_LEVEL = BUTTON_ACTIVE_HIGH ? HIGH : LOW;
constexpr int BUTTON_RELEASED_LEVEL = BUTTON_ACTIVE_HIGH ? LOW : HIGH;

// INMP441 - verified wiring (see README "Verified microphone configuration").
constexpr uint8_t I2S_SCK = 6;
constexpr uint8_t I2S_WS = 5;
constexpr uint8_t I2S_SD = 4;

}  // namespace Pins

#endif
