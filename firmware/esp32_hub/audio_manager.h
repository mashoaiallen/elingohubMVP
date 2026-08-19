#ifndef ELINGO_AUDIO_MANAGER_H
#define ELINGO_AUDIO_MANAGER_H

#include <Arduino.h>
#include <driver/i2s.h>

struct AudioChunk {
  AudioChunk() : data(nullptr), length(0), durationMs(0) {}

  uint8_t *data;
  size_t length;
  uint32_t durationMs;

  bool valid() const {
    return data != nullptr && length > 0;
  }
};

class AudioManager {
 public:
  AudioManager();

  bool begin();
  bool start();
  void stop();
  bool update();

  bool hasChunk() const;
  AudioChunk takeChunk();
  void releaseChunk(AudioChunk &chunk);
  const String &lastError() const;
  bool isRecording() const;
  bool hasError() const;

 private:
  enum class I2sSlot : uint8_t {
    LEFT = 0,
    RIGHT = 1
  };

  bool configureI2s();
  bool allocateChunkBuffer();
  void clearCurrentChunk();
  void writeWavHeader(uint32_t pcmBytes);
  bool finalizeChunk();
  bool restartFlatCaptureOnAlternateSlot();
  bool startI2sCapture();
  bool isFlatCapture(uint32_t rms) const;
  const char *slotName(I2sSlot slot) const;
  i2s_channel_fmt_t channelFormat(I2sSlot slot) const;
  int16_t convertRawSample(int32_t rawSample) const;
  void updateStats(int32_t rawSample, int16_t sample);
  void writeSample(int16_t sample);
  void resetChunkStats();
  uint32_t targetPcmBytes() const;
  uint32_t chunkCapacityBytes() const;
  void logDebugRead(size_t rawWordsRead,
                    const int16_t *pcmSamples,
                    size_t pcmSampleCount) const;

  bool initialized_;
  bool i2sDriverInstalled_;
  bool recording_;
  bool chunkReady_;
  bool retriedAlternateSlot_;
  bool sampleWindowPrinted_;
  I2sSlot currentSlot_;
  uint8_t *chunkBuffer_;
  size_t chunkLength_;
  uint32_t pcmBytesWritten_;
  uint32_t chunkStartedMs_;
  int32_t *i2sReadBuffer_;
  String lastError_;
  int32_t minRaw_;
  int32_t maxRaw_;
  int32_t minSample_;
  int32_t maxSample_;
  int64_t sumSamples_;
  uint64_t sumSquares_;
  uint32_t sampleCount_;
  uint32_t rawZeroCount_;
  uint32_t rawMinusOneCount_;
  uint32_t pcmZeroCount_;
};

#endif
