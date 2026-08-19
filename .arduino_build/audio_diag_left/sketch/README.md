#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\README.md"
# eLingo Hub ESP32-S3 Firmware

Arduino firmware for the eLingo Hub MVP hardware. This project is isolated from the Flask web app and communicates with the existing backend API over WiFi.

## Project Layout

```text
firmware/esp32_hub/
  eLingoHub.ino
  config.h / config.cpp
  pins.h
  wifi_manager.*
  lcd_manager.*
  led_manager.*
  button_manager.*
  api_manager.*
  session_manager.*
  audio_manager.*
  state_machine.*
```

## Required Libraries

Install these in Arduino IDE:

- ArduinoJson
- LiquidCrystal_I2C

Provided by the ESP32 Arduino core:

- WiFi
- HTTPClient
- Wire
- driver/i2s

## Board

- Board: Waveshare ESP32-S3-DEV-KIT-N32R16V
- Framework: Arduino
- Arduino IDE board fallback: ESP32S3 Dev Module
- Flash size: 32 MB
- PSRAM: enabled, preferably OPI PSRAM
- USB CDC on boot: enabled if you want Serial Monitor over USB
- Upload speed: 921600 or lower if uploads are unstable

## Wiring

| Component | Signal | ESP32-S3 GPIO |
| --- | --- | --- |
| Red LED | Anode through resistor | GPIO11 |
| Green LED | Anode through resistor | GPIO12 |
| Button | One side to GPIO, other to GND | GPIO10 |
| LCD I2C | SDA | GPIO8 |
| LCD I2C | SCL | GPIO9 |
| INMP441 | SCK/BCLK | GPIO4 |
| INMP441 | WS/LRCLK | GPIO5 |
| INMP441 | SD/DOUT | GPIO6 |

The button uses `INPUT_PULLUP`, so a press pulls GPIO10 to ground.

## Configure WiFi and Backend

Edit [config.cpp](config.cpp):

```cpp
const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
const char API_BASE_URL[] = "http://192.168.1.100:5000";
```

Use the LAN IP address of the computer running Flask. Do not use `127.0.0.1`; from the ESP32, that means the ESP32 itself.

Start the backend:

```bash
python app.py
```

The firmware expects these existing endpoints:

- `GET /api/health`
- `POST /api/session/start`
- `POST /api/session/audio`
- `POST /api/session/end`

## Upload Procedure

1. Open `firmware/esp32_hub/eLingoHub.ino` in Arduino IDE.
2. Select the ESP32-S3 board.
3. Enable PSRAM.
4. Install `ArduinoJson` and `LiquidCrystal_I2C`.
5. Update `config.cpp` with WiFi and backend URL.
6. Connect the board by USB.
7. Upload.
8. Open Serial Monitor at `115200` baud.

## Runtime Behavior

- Power on: LCD shows `eLingo Hub` / `Booting...`, red LED on.
- WiFi connect: LCD shows `Connecting...`.
- Backend online: LCD shows `eLingo Hub` / `Ready`, red LED on.
- Backend offline: LCD shows `Server` / `Offline`, retrying every five seconds.
- Button press while ready: starts a backend session and shows `Join` / session ID for ten seconds.
- Recording: green LED on, red off, LCD shows `REC` / session ID.
- Audio: records five-second WAV chunks and uploads them as multipart form data.
- Button press while recording: sends `/api/session/end`, resets counters, and returns to ready.
