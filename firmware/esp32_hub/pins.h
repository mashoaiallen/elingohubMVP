#ifndef ELINGO_PINS_H
#define ELINGO_PINS_H

#include <Arduino.h>

namespace Pins {

constexpr uint8_t RED_LED = 11;
constexpr uint8_t GREEN_LED = 12;

constexpr uint8_t BUTTON = 10;

constexpr uint8_t LCD_SDA = 7;
constexpr uint8_t LCD_SCL = 15;

constexpr uint8_t I2S_SCK = 6;
constexpr uint8_t I2S_WS = 5;
constexpr uint8_t I2S_SD = 4;

}  // namespace Pins

#endif
