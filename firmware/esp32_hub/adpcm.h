#ifndef ELINGO_ADPCM_H
#define ELINGO_ADPCM_H

#include <stddef.h>
#include <stdint.h>

// IMA ADPCM (WAVE_FORMAT_IMA_ADPCM, 4 bits/sample) - the Hub's upload format since
// firmware 0.3.0: 4x smaller than 16-bit PCM, which the 5.7 KB TCP window of this core needs
// (2026-11-12 field data: PCM uploads fell 47-71 s behind with two languages listening).
// Identical algorithm and container to supabase/functions/_shared/audio.ts (byte-for-byte; see
// tools/adpcm-check). No Arduino dependencies, so it also compiles on a PC for that check.
namespace Adpcm {

constexpr uint16_t BLOCK_ALIGN = 512;
constexpr uint16_t SAMPLES_PER_BLOCK = (BLOCK_ALIGN - 4) * 2 + 1;  // 1017
constexpr size_t WAV_HEADER_BYTES = 60;                            // RIFF + fmt(20) + fact + data

// Size of a complete WAV (header included) for `samples` mono samples.
size_t wavBytes(uint32_t samples);

// Encodes `samples` 16-bit mono samples into `out` (capacity >= wavBytes(samples)) as a WAV.
// Returns the number of bytes written.
size_t encodeWav(const int16_t *pcm, uint32_t samples, uint32_t sampleRate, uint8_t *out);

}  // namespace Adpcm

#endif
