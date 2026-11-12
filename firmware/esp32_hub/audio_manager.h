#ifndef ELINGO_AUDIO_MANAGER_H
#define ELINGO_AUDIO_MANAGER_H

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

// A finished 16 kHz / 16-bit / mono WAV chunk in PSRAM. Ownership passes to whoever takes it
// from the queue; give it back with AudioManager::releaseChunk().
struct AudioChunk {
  uint8_t *data = nullptr;
  size_t length = 0;
  uint32_t durationMs = 0;
  uint32_t seq = 0;
  int64_t capturedEpochMs = 0;  // 0 when the clock is not yet synced
  uint32_t rms = 0;
  int32_t peak = 0;
  bool silent = false;
};

// Continuous INMP441 capture on its own FreeRTOS task (core 1). Capture never pauses for
// network I/O: finished chunks go into a bounded queue (CAPTURE_QUEUE_DEPTH, ~60 s). When the
// queue is full the oldest chunk is dropped and counted.
class AudioManager {
 public:
  AudioManager();

  bool begin();
  bool startCapture(uint32_t firstSeq);
  // Stops capture and waits for the task to flush the partial tail chunk (>= 1 s) into the queue.
  void stopCapture();
  bool isCapturing() const;
  bool hardwareFault() const;

  AudioChunk *takeChunk(uint32_t waitMs);
  void releaseChunk(AudioChunk *chunk);
  void discardQueued();

  uint32_t queuedChunks() const;
  uint32_t droppedChunks() const;
  uint32_t capturedChunks() const;
  const char *lastError() const;

 private:
  enum class I2sSlot : uint8_t { LEFT = 0, RIGHT = 1 };

  static void taskEntry(void *arg);
  void taskLoop();
  void beginRun();
  void endRun();
  void processSamples(const int32_t *raw, size_t count);
  bool allocateChunk();
  void finalizeChunk(bool tail);
  void enqueue(AudioChunk *chunk);
  void writeWavHeader(uint8_t *buffer, uint32_t pcmBytes) const;
  void resetChunkStats();
  void setError(const char *message);

  bool configureI2s();
  i2s_channel_fmt_t channelFormat(I2sSlot slot) const;
  const char *slotName(I2sSlot slot) const;
  int16_t convertRawSample(int32_t rawSample) const;
  int16_t applyDsp(int16_t sample);
  uint32_t targetPcmBytes() const;

  TaskHandle_t task_;
  QueueHandle_t queue_;
  std::atomic<bool> captureRequested_;
  std::atomic<bool> running_;
  std::atomic<bool> hardwareFault_;
  std::atomic<uint32_t> dropped_;
  std::atomic<uint32_t> captured_;
  std::atomic<uint32_t> firstSeq_;

  bool i2sDriverInstalled_;
  I2sSlot currentSlot_;
  bool slotCheckedThisRun_;
  int32_t *readBuffer_;
  AudioChunk *current_;
  uint32_t pcmBytesWritten_;
  uint32_t nextSeq_;
  uint32_t discardUntilMs_;
  uint32_t consecutiveReadErrors_;

  float hpfPreviousInput_;
  float hpfPreviousOutput_;
  uint64_t sumSquares_;
  uint32_t sampleCount_;
  int32_t peak_;

  char lastError_[64];
};

#endif
