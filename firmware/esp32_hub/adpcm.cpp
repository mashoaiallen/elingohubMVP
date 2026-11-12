#include "adpcm.h"

#include <string.h>

namespace {

const int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void put16(uint8_t *b, uint16_t v) {
  b[0] = static_cast<uint8_t>(v & 0xFF);
  b[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *b, uint32_t v) {
  b[0] = static_cast<uint8_t>(v & 0xFF);
  b[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  b[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  b[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

size_t dataBytes(uint32_t samples) {
  if (samples == 0) {
    return 0;
  }
  uint32_t full = samples / Adpcm::SAMPLES_PER_BLOCK;
  uint32_t rest = samples % Adpcm::SAMPLES_PER_BLOCK;
  size_t bytes = static_cast<size_t>(full) * Adpcm::BLOCK_ALIGN;
  if (rest > 0) {
    bytes += 4 + rest / 2;  // header + ceil((rest - 1) / 2)
  }
  return bytes;
}

}  // namespace

namespace Adpcm {

size_t wavBytes(uint32_t samples) {
  return WAV_HEADER_BYTES + dataBytes(samples);
}

size_t encodeWav(const int16_t *pcm, uint32_t samples, uint32_t sampleRate, uint8_t *out) {
  size_t data = dataBytes(samples);
  memcpy(out + 0, "RIFF", 4);
  put32(out + 4, static_cast<uint32_t>(52 + data));
  memcpy(out + 8, "WAVE", 4);
  memcpy(out + 12, "fmt ", 4);
  put32(out + 16, 20);
  put16(out + 20, 0x11);  // WAVE_FORMAT_IMA_ADPCM
  put16(out + 22, 1);
  put32(out + 24, sampleRate);
  put32(out + 28, static_cast<uint32_t>((static_cast<uint64_t>(sampleRate) * BLOCK_ALIGN) / SAMPLES_PER_BLOCK));
  put16(out + 32, BLOCK_ALIGN);
  put16(out + 34, 4);
  put16(out + 36, 2);
  put16(out + 38, SAMPLES_PER_BLOCK);
  memcpy(out + 40, "fact", 4);
  put32(out + 44, 4);
  put32(out + 48, samples);
  memcpy(out + 52, "data", 4);
  put32(out + 56, static_cast<uint32_t>(data));

  uint8_t *dst = out + WAV_HEADER_BYTES;
  memset(dst, 0, data);
  int index = 0;
  for (uint32_t start = 0; start < samples; start += SAMPLES_PER_BLOCK) {
    uint32_t count = samples - start < SAMPLES_PER_BLOCK ? samples - start : SAMPLES_PER_BLOCK;
    uint8_t *block = dst + (start / SAMPLES_PER_BLOCK) * BLOCK_ALIGN;
    int32_t predictor = pcm[start];
    put16(block, static_cast<uint16_t>(static_cast<int16_t>(predictor)));
    block[2] = static_cast<uint8_t>(index);
    block[3] = 0;
    for (uint32_t i = 1; i < count; ++i) {
      int32_t diff = static_cast<int32_t>(pcm[start + i]) - predictor;
      uint8_t nibble = 0;
      if (diff < 0) {
        nibble = 8;
        diff = -diff;
      }
      int32_t step = kStep[index];
      int32_t delta = step >> 3;
      if (diff >= step) {
        nibble |= 4;
        diff -= step;
        delta += step;
      }
      step >>= 1;
      if (diff >= step) {
        nibble |= 2;
        diff -= step;
        delta += step;
      }
      step >>= 1;
      if (diff >= step) {
        nibble |= 1;
        delta += step;
      }
      predictor += (nibble & 8) ? -delta : delta;
      if (predictor > 32767) {
        predictor = 32767;
      } else if (predictor < -32768) {
        predictor = -32768;
      }
      index += kIndex[nibble];
      if (index < 0) {
        index = 0;
      } else if (index > 88) {
        index = 88;
      }
      uint8_t *pos = block + 4 + ((i - 1) >> 1);
      *pos |= ((i - 1) & 1) ? static_cast<uint8_t>(nibble << 4) : nibble;
    }
  }
  return WAV_HEADER_BYTES + data;
}

}  // namespace Adpcm
