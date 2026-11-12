#include "segment_store.h"

#include <cstring>

namespace {
class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};
}  // namespace

SegmentStore::SegmentStore() : mutex_(xSemaphoreCreateMutex()), bytesUsed_(0) {
  memset(rings_, 0, sizeof(rings_));
}

SegmentStore::Ring *SegmentStore::ringFor(const char *lang, bool create) {
  for (Ring &r : rings_) {
    if (r.lang[0] != '\0' && strcmp(r.lang, lang) == 0) {
      return &r;
    }
  }
  if (!create) {
    return nullptr;
  }
  for (Ring &r : rings_) {
    if (r.lang[0] == '\0') {
      strncpy(r.lang, lang, sizeof(r.lang) - 1);
      r.count = 0;
      return &r;
    }
  }
  return nullptr;
}

const SegmentStore::Ring *SegmentStore::ringFor(const char *lang) const {
  for (const Ring &r : rings_) {
    if (r.lang[0] != '\0' && strcmp(r.lang, lang) == 0) {
      return &r;
    }
  }
  return nullptr;
}

void SegmentStore::dropLocked(Segment *segment) {
  bytesUsed_ -= segment->length;
  if (segment->refs == 0) {
    free(segment->data);
    delete segment;
  } else {
    segment->evicted = true;  // freed by the last release()
  }
}

void SegmentStore::evictLocked(Ring &ring, uint8_t index) {
  Segment *segment = ring.items[index];
  for (uint8_t i = index; i + 1 < ring.count; ++i) {
    ring.items[i] = ring.items[i + 1];
  }
  ring.count--;
  dropLocked(segment);
}

bool SegmentStore::fresh(const Segment *segment, uint32_t now) {
  return now - segment->receivedMs < Config::SEGMENT_MAX_AGE_MS;
}

void SegmentStore::pruneStaleLocked(uint32_t now) {
  for (Ring &r : rings_) {
    // Rings are ordered by seq, which follows arrival closely; drop stale entries from the front.
    while (r.count > 0 && !fresh(r.items[0], now)) {
      evictLocked(r, 0);
    }
  }
}

bool SegmentStore::evictOldestLocked() {
  Ring *oldestRing = nullptr;
  for (Ring &r : rings_) {
    if (r.count > 0 && (oldestRing == nullptr || r.items[0]->receivedMs < oldestRing->items[0]->receivedMs)) {
      oldestRing = &r;
    }
  }
  if (oldestRing == nullptr) {
    return false;
  }
  evictLocked(*oldestRing, 0);
  return true;
}

bool SegmentStore::put(const char *lang, int64_t id, uint32_t seq, uint32_t durationMs, uint8_t *data,
                       size_t length) {
  Lock lock(mutex_);
  Ring *ring = ringFor(lang, true);
  if (ring == nullptr || length > Config::MAX_SEGMENT_BYTES) {
    free(data);
    return false;
  }
  for (uint8_t i = 0; i < ring->count; ++i) {
    if (ring->items[i]->seq == seq) {  // duplicate download
      free(data);
      return true;
    }
  }
  pruneStaleLocked(millis());
  while (bytesUsed_ + length > Config::SEGMENT_MEMORY_BUDGET && evictOldestLocked()) {
  }
  if (ring->count >= Config::SEGMENTS_PER_LANGUAGE) {
    evictLocked(*ring, 0);
  }
  Segment *segment = new Segment();
  segment->id = id;
  segment->seq = seq;
  segment->durationMs = durationMs;
  segment->receivedMs = millis();
  segment->data = data;
  segment->length = length;
  segment->refs = 0;
  segment->evicted = false;
  strncpy(segment->lang, lang, sizeof(segment->lang) - 1);
  segment->lang[sizeof(segment->lang) - 1] = '\0';

  // Keep each ring ordered by seq (segments can arrive slightly out of order).
  uint8_t pos = ring->count;
  while (pos > 0 && ring->items[pos - 1]->seq > seq) {
    ring->items[pos] = ring->items[pos - 1];
    --pos;
  }
  ring->items[pos] = segment;
  ring->count++;
  bytesUsed_ += length;
  return true;
}

Segment *SegmentStore::acquire(const char *lang, uint32_t seq) {
  Lock lock(mutex_);
  Ring *ring = ringFor(lang, false);
  if (ring == nullptr) {
    return nullptr;
  }
  uint32_t now = millis();
  for (uint8_t i = 0; i < ring->count; ++i) {
    if (ring->items[i]->seq == seq && fresh(ring->items[i], now)) {
      ring->items[i]->refs++;
      return ring->items[i];
    }
  }
  return nullptr;
}

void SegmentStore::release(Segment *segment) {
  if (segment == nullptr) {
    return;
  }
  Lock lock(mutex_);
  if (segment->refs > 0) {
    segment->refs--;
  }
  if (segment->refs == 0 && segment->evicted) {
    free(segment->data);
    delete segment;
  }
}

uint8_t SegmentStore::list(const char *lang, int32_t afterSeq, JsonArray out) const {
  Lock lock(mutex_);
  const Ring *ring = ringFor(lang);
  if (ring == nullptr) {
    return 0;
  }
  uint8_t n = 0;
  uint32_t now = millis();
  for (uint8_t i = 0; i < ring->count; ++i) {
    const Segment *s = ring->items[i];
    if (static_cast<int32_t>(s->seq) > afterSeq && fresh(s, now)) {
      JsonObject o = out.add<JsonObject>();
      o["seq"] = s->seq;
      o["durationMs"] = s->durationMs;
      o["bytes"] = s->length;
      o["ageMs"] = now - s->receivedMs;  // the page only joins at a segment that is still recent
      ++n;
    }
  }
  return n;
}

int32_t SegmentStore::latestSeq(const char *lang) const {
  Lock lock(mutex_);
  const Ring *ring = ringFor(lang);
  if (ring == nullptr || ring->count == 0 || !fresh(ring->items[ring->count - 1], millis())) {
    return -1;
  }
  return static_cast<int32_t>(ring->items[ring->count - 1]->seq);
}

bool SegmentStore::hasAudio(const char *lang) const {
  return latestSeq(lang) >= 0;
}

void SegmentStore::clear() {
  Lock lock(mutex_);
  for (Ring &r : rings_) {
    while (r.count > 0) {
      evictLocked(r, r.count - 1);
    }
    r.lang[0] = '\0';
  }
  bytesUsed_ = 0;
}

size_t SegmentStore::bytesUsed() const {
  Lock lock(mutex_);
  return bytesUsed_;
}

uint32_t SegmentStore::segmentCount() const {
  Lock lock(mutex_);
  uint32_t n = 0;
  for (const Ring &r : rings_) {
    n += r.count;
  }
  return n;
}
