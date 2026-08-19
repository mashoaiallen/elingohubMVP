#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\platform_notes.md"
# Platform Notes

## Assumptions

- The Flask backend is reachable from the ESP32 over the same WiFi network.
- `API_BASE_URL` uses plain HTTP and a LAN IP address.
- The LCD backpack address is `0x27`. Some modules use `0x3F`; change `LCD_I2C_ADDRESS` in `config.h` if the display stays blank.
- The INMP441 is read as the left I2S channel. If your module is wired for the opposite channel and audio is silent, change `I2S_CHANNEL_FMT_ONLY_LEFT` in `audio_manager.cpp` to `I2S_CHANNEL_FMT_ONLY_RIGHT`.
- Audio is captured as 48 kHz, mono, 16-bit PCM WAV.

## Audio Buffering

The firmware stores each five-second WAV chunk in RAM/PSRAM and frees that buffer after a successful upload. This matches the requested dependency list without adding SD, SPIFFS, or LittleFS.

If an upload fails, the completed chunk is kept in memory and retried every five seconds. Because this is a single-buffer MVP implementation, capture pauses at a chunk boundary while a failed chunk is waiting to upload. A production build should add a persistent queue or double-buffered upload worker if lossless continuous capture is required during poor network conditions.

## Backend Compatibility

The firmware posts multipart form data with these fields:

- `sessionId`
- `chunkNumber`
- `audioFile`

The file field is sent as `audio/wav` with filenames like `chunk0001.wav`. No backend changes are required for the current Flask API.

## Arduino Core Notes

This project uses the ESP32 Arduino core legacy `driver/i2s` API because it is stable and widely supported in Arduino IDE projects. ESP32 Arduino core 3.x may emit deprecation warnings for the legacy I2S driver, but it remains available for compatibility.

Enable PSRAM for the Waveshare ESP32-S3-DEV-KIT-N32R16V. Without PSRAM, large multipart uploads may fail because a five-second WAV chunk and HTTP request body need to be in memory at the same time.
