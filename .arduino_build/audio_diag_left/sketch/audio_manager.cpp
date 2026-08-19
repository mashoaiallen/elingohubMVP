#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\audio_manager.cpp"
#include "audio_manager.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

#include "config.h"
#include "pins.h"

// Diagnostic builds print one DMA read so flat 0/-1 captures are visible.
#if defined(ELINGO_AUDIO_DIAGNOSTIC) && ELINGO_AUDIO_DIAGNOSTIC
#define AUDIO_DEBUG
#endif

#if !defined(ELINGO_I2S_SLOT)
#define ELINGO_I2S_SLOT 0
#endif

#if !defined(ELINGO_PCM16_RIGHT_SHIFT)
#define ELINGO_PCM16_RIGHT_SHIFT 14
#endif

namespace
{

  constexpr size_t WAV_HEADER_BYTES = 44;
  constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
  constexpr uint16_t I2S_DMA_BUFFER_COUNT = 8;
  constexpr uint16_t I2S_DMA_BUFFER_LENGTH = 1024;
  constexpr uint8_t PCM16_RIGHT_SHIFT = ELINGO_PCM16_RIGHT_SHIFT;

#if ELINGO_I2S_SLOT == 1
  constexpr i2s_channel_fmt_t I2S_CHANNEL_FORMAT = I2S_CHANNEL_FMT_ONLY_RIGHT;
  constexpr const char *I2S_SLOT_NAME = "RIGHT";
#else
  constexpr i2s_channel_fmt_t I2S_CHANNEL_FORMAT = I2S_CHANNEL_FMT_ONLY_LEFT;
  constexpr const char *I2S_SLOT_NAME = "LEFT";
#endif

#ifdef AUDIO_DEBUG
  bool debugReadPrinted = false;
#endif

  uint8_t *allocateBytes(size_t length)
  {
    uint8_t *buffer = nullptr;

#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
    buffer = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer != nullptr)
    {
      return buffer;
    }
#endif

    return static_cast<uint8_t *>(malloc(length));
  }

  void releaseBytes(void *buffer)
  {
    free(buffer);
  }

  void writeLe16(uint8_t *target, uint16_t value)
  {
    target[0] = static_cast<uint8_t>(value & 0xFF);
    target[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }

  void writeLe32(uint8_t *target, uint32_t value)
  {
    target[0] = static_cast<uint8_t>(value & 0xFF);
    target[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    target[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    target[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
  }

#ifdef AUDIO_DEBUG
  void printHex32(uint32_t value)
  {
    char buffer[11];
    snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    Serial.println(buffer);
  }
#endif

} // namespace

AudioManager::AudioManager()
    : initialized_(false),
      recording_(false),
      chunkReady_(false),
      chunkBuffer_(nullptr),
      chunkLength_(0),
      pcmBytesWritten_(0),
      chunkStartedMs_(0),
      i2sReadBuffer_(nullptr),
      lastError_("not initialized"),
      minSample_(INT16_MAX),
      maxSample_(INT16_MIN),
      sumSamples_(0),
      sumSquares_(0),
      sampleCount_(0) {}

bool AudioManager::begin()
{
  if (initialized_)
  {
    return true;
  }

  i2sReadBuffer_ = reinterpret_cast<int32_t *>(
      allocateBytes(Config::I2S_READ_SAMPLES * sizeof(int32_t)));
  if (i2sReadBuffer_ == nullptr)
  {
    lastError_ = "i2s read buffer allocation failed";
    Serial.println(lastError_);
    return false;
  }

  if (!configureI2s())
  {
    releaseBytes(i2sReadBuffer_);
    i2sReadBuffer_ = nullptr;
    return false;
  }

  initialized_ = true;
  lastError_ = "";
  Serial.println("[Audio] I2S initialized");
  Serial.print("[Audio] Format: ");
  Serial.print(Config::AUDIO_SAMPLE_RATE);
  Serial.print(" Hz, ");
  Serial.print(Config::AUDIO_BITS_PER_SAMPLE);
  Serial.print("-bit, ");
  Serial.print(Config::AUDIO_CHANNELS);
  Serial.println(" channel WAV");
  Serial.print("[Audio] I2S RX: ");
  Serial.print(Config::AUDIO_SAMPLE_RATE);
  Serial.print(" Hz, 32-bit mono ");
  Serial.print(I2S_SLOT_NAME);
  Serial.print(" slot, raw samples shifted right by ");
  Serial.println(PCM16_RIGHT_SHIFT);
  return true;
}

bool AudioManager::start()
{
  if (!initialized_ && !begin())
  {
    return false;
  }

  clearCurrentChunk();
  if (!allocateChunkBuffer())
  {
    return false;
  }

  pcmBytesWritten_ = 0;
  chunkLength_ = 0;
  chunkReady_ = false;
  resetChunkStats();
  lastError_ = "";
#ifdef AUDIO_DEBUG
  debugReadPrinted = false;
#endif
  recording_ = true;
  chunkStartedMs_ = millis();

  esp_err_t zeroResult = i2s_zero_dma_buffer(I2S_PORT);
  if (zeroResult != ESP_OK)
  {
    lastError_ = "i2s zero dma buffer failed";
    Serial.print("[Audio] I2S zero DMA buffer failed: ");
    Serial.println(zeroResult);
    recording_ = false;
    clearCurrentChunk();
    return false;
  }

  esp_err_t startResult = i2s_start(I2S_PORT);
  if (startResult != ESP_OK)
  {
    lastError_ = "i2s start failed";
    Serial.print("[Audio] I2S start failed: ");
    Serial.println(startResult);
    recording_ = false;
    clearCurrentChunk();
    return false;
  }

  Serial.println("[Audio] Recording started");
  return true;
}

void AudioManager::stop()
{
  if (recording_)
  {
    i2s_stop(I2S_PORT);
  }

  recording_ = false;
  chunkReady_ = false;
  pcmBytesWritten_ = 0;
  chunkLength_ = 0;
  clearCurrentChunk();
  Serial.println("Audio capture stopped");
}

bool AudioManager::isRecording() const
{
  return recording_;
}

bool AudioManager::hasError() const
{
  return lastError_.length() > 0;
}

bool AudioManager::update()
{
  if (!recording_ || chunkReady_ || chunkBuffer_ == nullptr)
  {
    return false;
  }

  size_t bytesRead = 0;
  esp_err_t result = i2s_read(
      I2S_PORT,
      i2sReadBuffer_,
      Config::I2S_READ_SAMPLES * sizeof(int32_t),
      &bytesRead,
      pdMS_TO_TICKS(100));

  if (result == ESP_ERR_TIMEOUT)
  {
    Serial.println("[Audio] I2S read timed out; waiting for more samples");
    return false;
  }

  if (result != ESP_OK)
  {
    lastError_ = "i2s read failed";
    Serial.print("[Audio] I2S read failed: ");
    Serial.println(result);
    return false;
  }

  if (bytesRead == 0)
  {
    Serial.println("[Audio] I2S read returned zero bytes; waiting for more samples");
    return false;
  }

  lastError_ = "";

  size_t rawWordsRead = bytesRead / sizeof(int32_t);

#ifdef AUDIO_DEBUG
  Serial.print("[Audio] I2S read ok, bytes: ");
  Serial.print(bytesRead);
  Serial.print(", elapsed ms: ");
  Serial.println(millis() - chunkStartedMs_);

  int16_t debugPcmSamples[Config::AUDIO_DEBUG_SAMPLE_COUNT];
  size_t debugPcmCount = 0;
#endif

  if ((bytesRead % sizeof(int32_t)) != 0)
  {
    Serial.println("[Audio] Warning: dropping partial I2S sample");
  }

  for (size_t rawIndex = 0;
       rawIndex < rawWordsRead && pcmBytesWritten_ < targetPcmBytes();
       ++rawIndex)
  {
    int16_t pcmSample = convertRawSample(i2sReadBuffer_[rawIndex]);
    updateStats(pcmSample);

#ifdef AUDIO_DEBUG
    if (debugPcmCount < Config::AUDIO_DEBUG_SAMPLE_COUNT)
    {
      debugPcmSamples[debugPcmCount++] = pcmSample;
    }
#endif

    writeSample(pcmSample);
  }

#ifdef AUDIO_DEBUG
  if (!debugReadPrinted)
  {
    logDebugRead(rawWordsRead, debugPcmSamples, debugPcmCount);
    debugReadPrinted = true;
  }
#endif

  if (pcmBytesWritten_ >= targetPcmBytes())
  {
    return finalizeChunk();
  }

  return false;
}

bool AudioManager::hasChunk() const
{
  return chunkReady_ && chunkBuffer_ != nullptr && chunkLength_ > 0;
}

AudioChunk AudioManager::takeChunk()
{
  AudioChunk chunk;
  if (!hasChunk())
  {
    return chunk;
  }

  chunk.data = chunkBuffer_;
  chunk.length = chunkLength_;
  chunk.durationMs = Config::AUDIO_CHUNK_MS;

  chunkBuffer_ = nullptr;
  chunkLength_ = 0;
  pcmBytesWritten_ = 0;
  chunkReady_ = false;

  return chunk;
}

void AudioManager::releaseChunk(AudioChunk &chunk)
{
  if (chunk.data != nullptr)
  {
    releaseBytes(chunk.data);
  }

  chunk.data = nullptr;
  chunk.length = 0;
  chunk.durationMs = 0;
}

const String &AudioManager::lastError() const
{
  return lastError_;
}

bool AudioManager::configureI2s()
{
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = Config::AUDIO_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FORMAT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = I2S_DMA_BUFFER_COUNT;
  i2sConfig.dma_buf_len = I2S_DMA_BUFFER_LENGTH;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = false;
  i2sConfig.fixed_mclk = 0;

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.bck_io_num = Pins::I2S_SCK;
  pinConfig.ws_io_num = Pins::I2S_WS;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = Pins::I2S_SD;

  esp_err_t result = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);
  if (result != ESP_OK)
  {
    lastError_ = "i2s driver install failed";
    Serial.print(lastError_);
    Serial.print(": ");
    Serial.println(result);
    return false;
  }

  result = i2s_set_pin(I2S_PORT, &pinConfig);
  if (result != ESP_OK)
  {
    lastError_ = "i2s pin config failed";
    Serial.print(lastError_);
    Serial.print(": ");
    Serial.println(result);
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }

  result = i2s_zero_dma_buffer(I2S_PORT);
  if (result != ESP_OK)
  {
    lastError_ = "i2s zero dma buffer failed";
    Serial.print(lastError_);
    Serial.print(": ");
    Serial.println(result);
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }

  i2s_stop(I2S_PORT);
  Serial.println("[Audio] I2S driver configured");
  return true;
}

bool AudioManager::allocateChunkBuffer()
{
  chunkBuffer_ = allocateBytes(chunkCapacityBytes());
  if (chunkBuffer_ == nullptr)
  {
    lastError_ = "wav chunk allocation failed";
    Serial.println(lastError_);
    return false;
  }

  memset(chunkBuffer_, 0, WAV_HEADER_BYTES);
  return true;
}

void AudioManager::resetChunkStats()
{
  minSample_ = INT16_MAX;
  maxSample_ = INT16_MIN;
  sumSquares_ = 0;
  sumSamples_ = 0;
  sampleCount_ = 0;
}

void AudioManager::clearCurrentChunk()
{
  if (chunkBuffer_ != nullptr)
  {
    releaseBytes(chunkBuffer_);
    chunkBuffer_ = nullptr;
  }
}

void AudioManager::writeWavHeader(uint32_t pcmBytes)
{
  if (chunkBuffer_ == nullptr)
  {
    Serial.println("[Audio] Error: chunkBuffer_ is null in writeWavHeader");
    return;
  }

  uint32_t byteRate = Config::AUDIO_SAMPLE_RATE *
                      Config::AUDIO_CHANNELS *
                      (Config::AUDIO_BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = Config::AUDIO_CHANNELS *
                        (Config::AUDIO_BITS_PER_SAMPLE / 8);

  memcpy(chunkBuffer_ + 0, "RIFF", 4);
  writeLe32(chunkBuffer_ + 4, 36 + pcmBytes);
  memcpy(chunkBuffer_ + 8, "WAVE", 4);
  memcpy(chunkBuffer_ + 12, "fmt ", 4);
  writeLe32(chunkBuffer_ + 16, 16);
  writeLe16(chunkBuffer_ + 20, 1);
  writeLe16(chunkBuffer_ + 22, Config::AUDIO_CHANNELS);
  writeLe32(chunkBuffer_ + 24, Config::AUDIO_SAMPLE_RATE);
  writeLe32(chunkBuffer_ + 28, byteRate);
  writeLe16(chunkBuffer_ + 32, blockAlign);
  writeLe16(chunkBuffer_ + 34, Config::AUDIO_BITS_PER_SAMPLE);
  memcpy(chunkBuffer_ + 36, "data", 4);
  writeLe32(chunkBuffer_ + 40, pcmBytes);
}

bool AudioManager::finalizeChunk()
{
  recording_ = false;
  i2s_stop(I2S_PORT);

  uint32_t rms = 0;
  int32_t average = 0;
  int32_t minSample = 0;
  int32_t maxSample = 0;
  if (sampleCount_ > 0)
  {
    float meanSquare = static_cast<float>(sumSquares_) / static_cast<float>(sampleCount_);
    rms = static_cast<uint32_t>(sqrtf(meanSquare));
    average = static_cast<int32_t>(sumSamples_ / static_cast<int64_t>(sampleCount_));
    minSample = minSample_;
    maxSample = maxSample_;
  }

  Serial.println("[Audio]");
  Serial.print("[Audio] Min: ");
  Serial.println(minSample);
  Serial.print("[Audio] Max: ");
  Serial.println(maxSample);
  Serial.print("[Audio] Average: ");
  Serial.println(average);
  Serial.print("[Audio] RMS: ");
  Serial.println(rms);

  if (sampleCount_ == 0)
  {
    Serial.println("[Audio] Warning: sample count is zero, uploading diagnostic chunk anyway");
  }
  else if (rms <= 1)
  {
    Serial.print("[Audio] Capture failed: RMS = ");
    Serial.println(rms);
  }
  else if (rms < Config::AUDIO_SILENCE_RMS_THRESHOLD)
  {
    Serial.print("[Audio] Warning: possible silence. RMS = ");
    Serial.println(rms);
  }
  else
  {
    Serial.println("[Audio] Capture contains non-silent PCM");
  }

  writeWavHeader(pcmBytesWritten_);
  chunkLength_ = WAV_HEADER_BYTES + pcmBytesWritten_;
  chunkReady_ = true;
  lastError_ = "";

  Serial.print("[Audio] WAV chunk ready bytes: ");
  Serial.println(chunkLength_);
  return true;
}

int16_t AudioManager::convertRawSample(int32_t rawSample) const
{
  return static_cast<int16_t>(rawSample >> PCM16_RIGHT_SHIFT);
}

void AudioManager::updateStats(int16_t sample)
{
  int32_t sampleValue = sample;
  minSample_ = std::min(minSample_, sampleValue);
  maxSample_ = std::max(maxSample_, sampleValue);
  sumSamples_ += sampleValue;

  int64_t square = static_cast<int64_t>(sampleValue) * static_cast<int64_t>(sampleValue);
  sumSquares_ += static_cast<uint64_t>(square);
  sampleCount_++;
}

void AudioManager::writeSample(int16_t sample)
{
  if (chunkBuffer_ == nullptr || pcmBytesWritten_ + sizeof(int16_t) > targetPcmBytes())
  {
    return;
  }

  size_t offset = WAV_HEADER_BYTES + pcmBytesWritten_;
  writeLe16(chunkBuffer_ + offset, static_cast<uint16_t>(sample));
  pcmBytesWritten_ += sizeof(int16_t);
}

uint32_t AudioManager::targetPcmBytes() const
{
  return Config::AUDIO_SAMPLE_RATE *
         Config::AUDIO_CHANNELS *
         (Config::AUDIO_BITS_PER_SAMPLE / 8) *
         Config::AUDIO_CHUNK_MS / 1000;
}

uint32_t AudioManager::chunkCapacityBytes() const
{
  return WAV_HEADER_BYTES + targetPcmBytes();
}

void AudioManager::logDebugRead(size_t rawWordsRead,
                                const int16_t *pcmSamples,
                                size_t pcmSampleCount) const
{
#ifndef AUDIO_DEBUG
  (void)rawWordsRead;
  (void)pcmSamples;
  (void)pcmSampleCount;
#else
  size_t rawToPrint = std::min(rawWordsRead, static_cast<size_t>(Config::AUDIO_DEBUG_SAMPLE_COUNT));
  size_t pcmToPrint = std::min(pcmSampleCount, static_cast<size_t>(Config::AUDIO_DEBUG_SAMPLE_COUNT));

  Serial.println("[Audio] RAW");
  for (size_t i = 0; i < rawToPrint; ++i)
  {
    printHex32(static_cast<uint32_t>(i2sReadBuffer_[i]));
  }

  Serial.println("[Audio] PCM");
  for (size_t i = 0; i < pcmToPrint; ++i)
  {
    Serial.println(pcmSamples[i]);
  }
#endif
}
