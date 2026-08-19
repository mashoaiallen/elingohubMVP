#ifndef ELINGO_CONFIG_H
#define ELINGO_CONFIG_H

#include <Arduino.h>

namespace Config {

extern const char WIFI_SSID[];
extern const char WIFI_PASSWORD[];
extern const char API_BASE_URL[];

constexpr uint8_t LCD_I2C_ADDRESS = 0x27;
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;

constexpr uint32_t BOOT_SCREEN_MS = 1200;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t HEALTH_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t JOIN_SCREEN_MS = 10000;
constexpr uint32_t AUDIO_CHUNK_MS = 5000;
constexpr uint32_t UPLOAD_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;

constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint16_t AUDIO_BITS_PER_SAMPLE = 16;
constexpr uint8_t AUDIO_CHANNELS = 1;
constexpr uint16_t I2S_READ_SAMPLES = 256;
constexpr uint32_t AUDIO_SILENCE_RMS_THRESHOLD = 32;
constexpr uint32_t AUDIO_FLAT_RMS_THRESHOLD = 1;
constexpr uint8_t AUDIO_DEBUG_SAMPLE_COUNT = 16;
constexpr uint8_t AUDIO_PCM_RIGHT_SHIFT = 14;
constexpr uint8_t AUDIO_PRIMARY_I2S_SLOT = 0;  // 0 = LEFT, 1 = RIGHT
constexpr bool AUDIO_AUTO_TRY_BOTH_I2S_SLOTS = true;
constexpr bool AUDIO_PRINT_SAMPLE_WINDOW = true;

String endpoint(const char *path);

}  // namespace Config

#endif
