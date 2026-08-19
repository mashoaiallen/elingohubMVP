#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\audio_manager.h"
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
  bool configureI2s();
  bool allocateChunkBuffer();
  void clearCurrentChunk();
  void writeWavHeader(uint32_t pcmBytes);
  bool finalizeChunk();
  int16_t convertRawSample(int32_t rawSample) const;
  void updateStats(int16_t sample);
  void writeSample(int16_t sample);
  void resetChunkStats();
  uint32_t targetPcmBytes() const;
  uint32_t chunkCapacityBytes() const;
  void logDebugRead(size_t rawWordsRead,
                    const int16_t *pcmSamples,
                    size_t pcmSampleCount) const;

  bool initialized_;
  bool recording_;
  bool chunkReady_;
  uint8_t *chunkBuffer_;
  size_t chunkLength_;
  uint32_t pcmBytesWritten_;
  uint32_t chunkStartedMs_;
  int32_t *i2sReadBuffer_;
  String lastError_;
  int32_t minSample_;
  int32_t maxSample_;
  int64_t sumSamples_;
  uint64_t sumSquares_;
  uint32_t sampleCount_;
};

#endif
