#include "audio_manager.h"

#include <esp_heap_caps.h>
#include <sys/time.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "pins.h"

namespace {

constexpr size_t WAV_HEADER_BYTES = 44;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
// I2S driver settings below are the verified configuration (unchanged from the working build).
constexpr uint16_t I2S_DMA_BUFFER_COUNT = 8;
constexpr uint16_t I2S_DMA_BUFFER_LENGTH = 1024;

// "DSP B": 80 Hz first-order high-pass + 1.5x gain (unchanged).
constexpr bool DSP_ENABLED = true;
constexpr float DSP_HPF_CUTOFF_HZ = 80.0f;
constexpr float DSP_GAIN = 1.5f;
constexpr float DSP_PI = 3.14159265358979323846f;
constexpr float DSP_HPF_RC_SECONDS = 1.0f / (2.0f * DSP_PI * DSP_HPF_CUTOFF_HZ);
constexpr float DSP_HPF_DT_SECONDS = 1.0f / static_cast<float>(Config::AUDIO_SAMPLE_RATE);
constexpr float DSP_HPF_ALPHA = DSP_HPF_RC_SECONDS / (DSP_HPF_RC_SECONDS + DSP_HPF_DT_SECONDS);

constexpr uint32_t MAX_CONSECUTIVE_READ_ERRORS = 50;  // ~5 s of failed reads => hardware fault

uint8_t *allocatePsram(size_t length) {
  void *p = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    p = malloc(length);
  }
  return static_cast<uint8_t *>(p);
}

void writeLe16(uint8_t *target, uint16_t value) {
  target[0] = static_cast<uint8_t>(value & 0xFF);
  target[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeLe32(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value & 0xFF);
  target[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  target[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  target[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

int16_t saturate16(int32_t value) {
  if (value > INT16_MAX) return INT16_MAX;
  if (value < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(value);
}

int16_t clampToPcm16(float value) {
  if (value > static_cast<float>(INT16_MAX)) return INT16_MAX;
  if (value < static_cast<float>(INT16_MIN)) return INT16_MIN;
  return static_cast<int16_t>(value);
}

int64_t epochMillis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) {
    return 0;
  }
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

}  // namespace

AudioManager::AudioManager()
    : task_(nullptr),
      queue_(nullptr),
      captureRequested_(false),
      running_(false),
      hardwareFault_(false),
      dropped_(0),
      captured_(0),
      firstSeq_(1),
      i2sDriverInstalled_(false),
      currentSlot_(Config::AUDIO_PRIMARY_I2S_SLOT == 1 ? I2sSlot::RIGHT : I2sSlot::LEFT),
      slotCheckedThisRun_(false),
      readBuffer_(nullptr),
      current_(nullptr),
      pcmBytesWritten_(0),
      nextSeq_(1),
      discardUntilMs_(0),
      consecutiveReadErrors_(0),
      hpfPreviousInput_(0.0f),
      hpfPreviousOutput_(0.0f),
      sumSquares_(0),
      sampleCount_(0),
      peak_(0) {
  lastError_[0] = '\0';
}

bool AudioManager::begin() {
  readBuffer_ = static_cast<int32_t *>(malloc(Config::I2S_READ_SAMPLES * sizeof(int32_t)));
  queue_ = xQueueCreate(Config::CAPTURE_QUEUE_DEPTH, sizeof(AudioChunk *));
  if (readBuffer_ == nullptr || queue_ == nullptr) {
    setError("audio buffer allocation failed");
    hardwareFault_ = true;
    return false;
  }
  if (!psramFound()) {
    setError("PSRAM not found (enable OPI PSRAM)");
    hardwareFault_ = true;
    return false;
  }
  if (!configureI2s()) {
    hardwareFault_ = true;
    return false;
  }
  if (xTaskCreatePinnedToCore(taskEntry, "audio", 6144, this, 10, &task_, 1) != pdPASS) {
    setError("audio task creation failed");
    hardwareFault_ = true;
    return false;
  }
  Serial.printf("[Audio] Ready: %lu Hz 16-bit mono, %s slot, pins SCK=%u WS=%u SD=%u, %lu ms chunks\n",
                static_cast<unsigned long>(Config::AUDIO_SAMPLE_RATE), slotName(currentSlot_), Pins::I2S_SCK,
                Pins::I2S_WS, Pins::I2S_SD, static_cast<unsigned long>(Config::AUDIO_CHUNK_MS));
  return true;
}

bool AudioManager::startCapture(uint32_t firstSeq) {
  if (hardwareFault_) {
    return false;
  }
  firstSeq_ = firstSeq == 0 ? 1 : firstSeq;
  captureRequested_ = true;
  xTaskNotifyGive(task_);
  return true;
}

void AudioManager::stopCapture() {
  captureRequested_ = false;
  if (task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
  uint32_t started = millis();
  while (running_ && millis() - started < 1000) {
    delay(5);
  }
}

bool AudioManager::isCapturing() const {
  return running_;
}

bool AudioManager::hardwareFault() const {
  return hardwareFault_;
}

AudioChunk *AudioManager::takeChunk(uint32_t waitMs) {
  AudioChunk *chunk = nullptr;
  if (queue_ != nullptr && xQueueReceive(queue_, &chunk, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
    return chunk;
  }
  return nullptr;
}

void AudioManager::releaseChunk(AudioChunk *chunk) {
  if (chunk == nullptr) {
    return;
  }
  free(chunk->data);
  delete chunk;
}

void AudioManager::discardQueued() {
  AudioChunk *chunk = nullptr;
  while (queue_ != nullptr && xQueueReceive(queue_, &chunk, 0) == pdTRUE) {
    releaseChunk(chunk);
  }
}

uint32_t AudioManager::queuedChunks() const {
  return queue_ != nullptr ? uxQueueMessagesWaiting(queue_) : 0;
}

uint32_t AudioManager::droppedChunks() const {
  return dropped_;
}

uint32_t AudioManager::capturedChunks() const {
  return captured_;
}

const char *AudioManager::lastError() const {
  return lastError_;
}

void AudioManager::setError(const char *message) {
  strncpy(lastError_, message, sizeof(lastError_) - 1);
  lastError_[sizeof(lastError_) - 1] = '\0';
  Serial.printf("[Audio] Error: %s\n", message);
}

void AudioManager::taskEntry(void *arg) {
  static_cast<AudioManager *>(arg)->taskLoop();
}

void AudioManager::taskLoop() {
  for (;;) {
    if (!captureRequested_) {
      if (running_) {
        endRun();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    if (!running_) {
      beginRun();
      if (!running_) {
        captureRequested_ = false;
        continue;
      }
    }

    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_PORT, readBuffer_, Config::I2S_READ_SAMPLES * sizeof(int32_t), &bytesRead,
                                pdMS_TO_TICKS(100));
    if (result != ESP_OK || bytesRead == 0) {
      if (++consecutiveReadErrors_ >= MAX_CONSECUTIVE_READ_ERRORS) {
        setError("I2S stopped delivering samples");
        hardwareFault_ = true;
        captureRequested_ = false;
      }
      continue;
    }
    consecutiveReadErrors_ = 0;

    if (static_cast<int32_t>(millis() - discardUntilMs_) < 0) {
      continue;  // INMP441 start-up burst
    }
    processSamples(readBuffer_, bytesRead / sizeof(int32_t));
  }
}

void AudioManager::beginRun() {
  nextSeq_ = firstSeq_;
  hpfPreviousInput_ = 0.0f;
  hpfPreviousOutput_ = 0.0f;
  slotCheckedThisRun_ = false;
  consecutiveReadErrors_ = 0;
  if (!allocateChunk()) {
    return;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  if (i2s_start(I2S_PORT) != ESP_OK) {
    setError("i2s start failed");
    releaseChunk(current_);
    current_ = nullptr;
    return;
  }
  discardUntilMs_ = millis() + Config::AUDIO_STARTUP_DISCARD_MS;
  running_ = true;
  Serial.printf("[Audio] Capture started at seq %lu (%s slot)\n", static_cast<unsigned long>(nextSeq_),
                slotName(currentSlot_));
}

void AudioManager::endRun() {
  i2s_stop(I2S_PORT);
  uint32_t tailMs = pcmBytesWritten_ * 1000UL /
                    (Config::AUDIO_SAMPLE_RATE * Config::AUDIO_CHANNELS * (Config::AUDIO_BITS_PER_SAMPLE / 8));
  if (current_ != nullptr && tailMs >= Config::AUDIO_MIN_TAIL_MS) {
    finalizeChunk(true);
  } else if (current_ != nullptr) {
    releaseChunk(current_);
    current_ = nullptr;
  }
  running_ = false;
  Serial.println("[Audio] Capture stopped");
}

bool AudioManager::allocateChunk() {
  AudioChunk *chunk = new AudioChunk();
  chunk->data = allocatePsram(WAV_HEADER_BYTES + targetPcmBytes());
  if (chunk->data == nullptr) {
    delete chunk;
    current_ = nullptr;
    setError("chunk allocation failed");
    return false;
  }
  chunk->capturedEpochMs = epochMillis();
  current_ = chunk;
  pcmBytesWritten_ = 0;
  resetChunkStats();
  return true;
}

void AudioManager::resetChunkStats() {
  sumSquares_ = 0;
  sampleCount_ = 0;
  peak_ = 0;
}

void AudioManager::processSamples(const int32_t *raw, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (current_ == nullptr && !allocateChunk()) {
      return;  // out of memory: drop samples until a chunk can be allocated
    }
    int16_t sample = applyDsp(convertRawSample(raw[i]));
    writeLe16(current_->data + WAV_HEADER_BYTES + pcmBytesWritten_, static_cast<uint16_t>(sample));
    pcmBytesWritten_ += sizeof(int16_t);

    int32_t v = sample;
    sumSquares_ += static_cast<uint64_t>(static_cast<int64_t>(v) * v);
    sampleCount_++;
    if (abs(v) > peak_) {
      peak_ = abs(v);
    }

    if (pcmBytesWritten_ >= targetPcmBytes()) {
      finalizeChunk(false);
      if (static_cast<int32_t>(millis() - discardUntilMs_) < 0) {
        return;  // slot was switched; the rest of this batch came from the old slot
      }
    }
  }
}

void AudioManager::finalizeChunk(bool tail) {
  AudioChunk *chunk = current_;
  current_ = nullptr;
  uint32_t rms = sampleCount_ > 0 ? static_cast<uint32_t>(sqrtf(static_cast<float>(sumSquares_) / sampleCount_)) : 0;

  // Verified behaviour kept: if the first chunk of a run is flat, try the other I2S slot once.
  if (!tail && !slotCheckedThisRun_) {
    slotCheckedThisRun_ = true;
    if (rms <= Config::AUDIO_FLAT_RMS_THRESHOLD && Config::AUDIO_AUTO_TRY_BOTH_I2S_SLOTS) {
      currentSlot_ = currentSlot_ == I2sSlot::LEFT ? I2sSlot::RIGHT : I2sSlot::LEFT;
      Serial.printf("[Audio] Flat capture; switching to %s slot\n", slotName(currentSlot_));
      releaseChunk(chunk);
      if (configureI2s() && i2s_start(I2S_PORT) == ESP_OK) {
        discardUntilMs_ = millis() + Config::AUDIO_STARTUP_DISCARD_MS;
        allocateChunk();
      } else {
        hardwareFault_ = true;
        captureRequested_ = false;
      }
      return;
    }
  }

  writeWavHeader(chunk->data, pcmBytesWritten_);
  chunk->length = WAV_HEADER_BYTES + pcmBytesWritten_;
  chunk->durationMs = pcmBytesWritten_ * 1000UL /
                      (Config::AUDIO_SAMPLE_RATE * Config::AUDIO_CHANNELS * (Config::AUDIO_BITS_PER_SAMPLE / 8));
  chunk->seq = nextSeq_++;
  chunk->rms = rms;
  chunk->peak = peak_;
  chunk->silent = rms < Config::AUDIO_SILENCE_RMS_THRESHOLD;
  captured_++;
  Serial.printf("[Audio] Chunk %lu ready: %u ms, RMS %lu, peak %ld%s\n", static_cast<unsigned long>(chunk->seq),
                static_cast<unsigned>(chunk->durationMs), static_cast<unsigned long>(rms),
                static_cast<long>(peak_), chunk->silent ? " (silent)" : "");
  enqueue(chunk);
  if (!tail) {
    allocateChunk();
  }
}

void AudioManager::enqueue(AudioChunk *chunk) {
  if (xQueueSend(queue_, &chunk, 0) == pdTRUE) {
    return;
  }
  AudioChunk *oldest = nullptr;
  if (xQueueReceive(queue_, &oldest, 0) == pdTRUE) {
    Serial.printf("[Audio] Queue full; dropping oldest chunk %lu\n", static_cast<unsigned long>(oldest->seq));
    releaseChunk(oldest);
    dropped_++;
  }
  if (xQueueSend(queue_, &chunk, 0) != pdTRUE) {
    releaseChunk(chunk);
    dropped_++;
  }
}

void AudioManager::writeWavHeader(uint8_t *buffer, uint32_t pcmBytes) const {
  uint32_t byteRate = Config::AUDIO_SAMPLE_RATE * Config::AUDIO_CHANNELS * (Config::AUDIO_BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = Config::AUDIO_CHANNELS * (Config::AUDIO_BITS_PER_SAMPLE / 8);
  memcpy(buffer + 0, "RIFF", 4);
  writeLe32(buffer + 4, 36 + pcmBytes);
  memcpy(buffer + 8, "WAVE", 4);
  memcpy(buffer + 12, "fmt ", 4);
  writeLe32(buffer + 16, 16);
  writeLe16(buffer + 20, 1);
  writeLe16(buffer + 22, Config::AUDIO_CHANNELS);
  writeLe32(buffer + 24, Config::AUDIO_SAMPLE_RATE);
  writeLe32(buffer + 28, byteRate);
  writeLe16(buffer + 32, blockAlign);
  writeLe16(buffer + 34, Config::AUDIO_BITS_PER_SAMPLE);
  memcpy(buffer + 36, "data", 4);
  writeLe32(buffer + 40, pcmBytes);
}

bool AudioManager::configureI2s() {
  if (i2sDriverInstalled_) {
    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    i2sDriverInstalled_ = false;
  }

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = Config::AUDIO_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = channelFormat(currentSlot_);
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

  if (i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr) != ESP_OK) {
    setError("i2s driver install failed");
    return false;
  }
  i2sDriverInstalled_ = true;
  if (i2s_set_pin(I2S_PORT, &pinConfig) != ESP_OK) {
    setError("i2s pin config failed");
    i2s_driver_uninstall(I2S_PORT);
    i2sDriverInstalled_ = false;
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  i2s_stop(I2S_PORT);
  return true;
}

i2s_channel_fmt_t AudioManager::channelFormat(I2sSlot slot) const {
  return slot == I2sSlot::RIGHT ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_ONLY_LEFT;
}

const char *AudioManager::slotName(I2sSlot slot) const {
  return slot == I2sSlot::RIGHT ? "RIGHT" : "LEFT";
}

int16_t AudioManager::convertRawSample(int32_t rawSample) const {
  // Same scaling as before (>> 14), but saturating: the previous int16 cast wrapped around on
  // loud input (98-1,125 wrap jumps per recorded session).
  return saturate16(rawSample >> Config::AUDIO_PCM_RIGHT_SHIFT);
}

int16_t AudioManager::applyDsp(int16_t sample) {
  if (!DSP_ENABLED) {
    return sample;
  }
  float input = static_cast<float>(sample);
  float filtered = DSP_HPF_ALPHA * (hpfPreviousOutput_ + input - hpfPreviousInput_);
  hpfPreviousInput_ = input;
  hpfPreviousOutput_ = filtered;
  return clampToPcm16(filtered * DSP_GAIN);
}

uint32_t AudioManager::targetPcmBytes() const {
  return Config::AUDIO_SAMPLE_RATE * Config::AUDIO_CHANNELS * (Config::AUDIO_BITS_PER_SAMPLE / 8) *
         Config::AUDIO_CHUNK_MS / 1000;
}
