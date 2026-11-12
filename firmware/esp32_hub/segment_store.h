#ifndef ELINGO_SEGMENT_STORE_H
#define ELINGO_SEGMENT_STORE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

// Recent translated audio per language, held in PSRAM. Downloaded once from the cloud and
// served to every phone listening to that language; each phone keeps its own playback cursor
// (seq), so nothing is consumed or removed by a read. Bounded by SEGMENTS_PER_LANGUAGE and
// SEGMENT_MEMORY_BUDGET; segments being served are reference-counted and freed afterwards.
// Segments older than SEGMENT_MAX_AGE_MS are hidden from phones and pruned: a phone that joins
// a language nobody has listened to for a while must not hear minutes-old audio.
struct Segment {
  int64_t id;
  uint32_t seq;
  uint32_t durationMs;
  uint32_t receivedMs;
  uint8_t *data;
  size_t length;
  uint16_t refs;
  bool evicted;
  char lang[8];
};

class SegmentStore {
 public:
  SegmentStore();

  // Takes ownership of data (allocated with malloc/heap_caps_malloc).
  bool put(const char *lang, int64_t id, uint32_t seq, uint32_t durationMs, uint8_t *data, size_t length);
  Segment *acquire(const char *lang, uint32_t seq);
  void release(Segment *segment);
  // Fills out with segments newer than afterSeq; returns the number written.
  uint8_t list(const char *lang, int32_t afterSeq, JsonArray out) const;
  int32_t latestSeq(const char *lang) const;
  bool hasAudio(const char *lang) const;
  void clear();
  size_t bytesUsed() const;
  uint32_t segmentCount() const;

 private:
  struct Ring {
    char lang[8];
    Segment *items[Config::SEGMENTS_PER_LANGUAGE];
    uint8_t count;
  };

  Ring *ringFor(const char *lang, bool create);
  const Ring *ringFor(const char *lang) const;
  static bool fresh(const Segment *segment, uint32_t now);
  void pruneStaleLocked(uint32_t now);
  void evictLocked(Ring &ring, uint8_t index);
  void dropLocked(Segment *segment);
  bool evictOldestLocked();

  mutable SemaphoreHandle_t mutex_;
  Ring rings_[Config::MAX_LANGUAGES];
  size_t bytesUsed_;
};

#endif
