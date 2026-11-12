#ifndef ELINGO_CONFIG_H
#define ELINGO_CONFIG_H

#include <Arduino.h>

// Compile-time constants. Credentials and per-site settings live in NVS (settings.h), seeded
// from the gitignored secrets.h on first boot.
namespace Config {

constexpr const char *FIRMWARE_VERSION = "0.3.1-m2";

// ---- Student network (soft-AP) ------------------------------------------------------------
constexpr const char *AP_SSID = "eLingo-Hub";
constexpr uint8_t AP_CHANNEL = 1;  // the radio moves to the upstream network's channel once joined
// This core is built with CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM=7, which caps the soft-AP at
// 17 - 7 = 10 stations (ESP-IDF Wi-Fi driver guide).
constexpr uint8_t AP_MAX_CLIENTS = 10;

// ---- Timing -------------------------------------------------------------------------------
constexpr uint32_t BOOT_SWEEP_MS = 900;
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t STA_RETRY_MIN_MS = 5000;
constexpr uint32_t STA_RETRY_MAX_MS = 60000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 5000;
constexpr uint32_t BUTTON_LOCKOUT_MS = 2000;
constexpr uint32_t SESSION_END_RED_MS = 5000;
constexpr uint32_t SESSION_START_TIMEOUT_MS = 20000;
constexpr uint32_t SETUP_MODE_MS = 10UL * 60UL * 1000UL;

constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t CLOUD_POLL_MS = 1000;
constexpr uint32_t LISTENER_SYNC_MS = 5000;
constexpr uint32_t LISTENER_SYNC_MIN_GAP_MS = 750;
constexpr uint32_t IDLE_HELLO_MS = 30000;
constexpr uint32_t CLOUD_RETRY_MIN_MS = 1000;
constexpr uint32_t CLOUD_RETRY_MAX_MS = 15000;
constexpr uint32_t UPLOAD_DEGRADED_AFTER_MS = 15000;
constexpr uint32_t END_FLUSH_MAX_MS = 10000;

// Phones heartbeat every 10 s; a listener that misses 3 heartbeats is dropped.
constexpr uint32_t LISTENER_TIMEOUT_MS = 30000;

// ---- Capacity / memory (PSRAM) --------------------------------------------------------------
constexpr uint8_t MAX_LISTENERS = 16;
constexpr uint8_t MAX_LANGUAGES = 8;
constexpr uint8_t SEGMENTS_PER_LANGUAGE = 12;               // ~60 s of recent audio per language
constexpr size_t MAX_SEGMENT_BYTES = 1024 * 1024;
constexpr size_t SEGMENT_MEMORY_BUDGET = 6 * 1024 * 1024;
constexpr uint8_t CAPTURE_QUEUE_DEPTH = 12;                 // ~60 s of source audio while offline

// ---- Live delivery: never play stale audio ----------------------------------------------------
// A segment is not downloaded when its source chunk is this many chunks older than the chunk
// being recorded now (8 x 5 s = 40 s behind live). Covers resume after a reboot, a language that
// had no listeners for a while, and upload backlogs.
constexpr uint32_t MAX_SEGMENT_LAG_CHUNKS = 8;
// Segments held on the Hub longer than this are no longer offered to phones.
constexpr uint32_t SEGMENT_MAX_AGE_MS = 60000;
// Consecutive chunks of digital silence (every sample 0) before the microphone is reported dead.
constexpr uint32_t MIC_DEAD_WARN_CHUNKS = 2;

// ---- Audio (verified configuration - do not change without hardware evidence) -------------
constexpr uint32_t AUDIO_CHUNK_MS = 5000;
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint16_t AUDIO_BITS_PER_SAMPLE = 16;
constexpr uint8_t AUDIO_CHANNELS = 1;
constexpr uint16_t I2S_READ_SAMPLES = 256;
constexpr uint32_t AUDIO_SILENCE_RMS_THRESHOLD = 32;
constexpr uint32_t AUDIO_FLAT_RMS_THRESHOLD = 1;
constexpr uint8_t AUDIO_PCM_RIGHT_SHIFT = 14;
constexpr uint8_t AUDIO_PRIMARY_I2S_SLOT = 0;  // 0 = LEFT, 1 = RIGHT
constexpr bool AUDIO_AUTO_TRY_BOTH_I2S_SLOTS = true;
// The INMP441 outputs a ~300 ms burst after its clock starts (measured in recorded chunks);
// those samples are discarded once per session start.
constexpr uint32_t AUDIO_STARTUP_DISCARD_MS = 300;
// On session end a partial chunk is kept if it holds at least this much audio.
constexpr uint32_t AUDIO_MIN_TAIL_MS = 1000;
// Upload chunks as IMA ADPCM (40 KB per 5 s instead of 160 KB). Capture itself is unchanged:
// still 16 kHz, 32-bit slots, >> 14, LEFT slot; only the transport encoding differs.
constexpr bool AUDIO_UPLOAD_ADPCM = true;

}  // namespace Config

#endif
