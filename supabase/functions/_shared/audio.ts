// Audio helpers shared by the Edge Functions: WAV parsing/building and IMA ADPCM.
//
// Transport format (Milestone 2): 16 kHz mono IMA ADPCM in a standard WAV container
// (WAVE_FORMAT_IMA_ADPCM = 0x11, 4 bits/sample, 512-byte blocks). It is 4x smaller than 16-bit
// PCM, which the Hub's 5.7 KB TCP window needs (measured 2026-11-12: PCM uploads fell 47-71 s
// behind with two languages). The Hub encodes it (firmware/esp32_hub/audio_manager.cpp), the
// worker decodes it for Deepgram and encodes TTS output with it, and the student page decodes
// it in JavaScript (firmware/esp32_hub/data/adpcm.js). All three implement the same algorithm.

export const WAVE_FORMAT_PCM = 1;
export const WAVE_FORMAT_IMA_ADPCM = 0x11;
export const ADPCM_BLOCK_ALIGN = 512;
export const ADPCM_SAMPLES_PER_BLOCK = (ADPCM_BLOCK_ALIGN - 4) * 2 + 1; // 1017

const STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
  29794, 32767,
];
const INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

export interface PcmAudio {
  sampleRate: number;
  samples: Int16Array; // mono
}

export interface WavInfo {
  format: number;
  channels: number;
  sampleRate: number;
  bitsPerSample: number;
  blockAlign: number;
  samplesPerBlock: number;
  totalSamples: number | null; // from the fact chunk (ADPCM)
  data: Uint8Array;
}

function u16(b: Uint8Array, o: number): number {
  return b[o] | (b[o + 1] << 8);
}
function u32(b: Uint8Array, o: number): number {
  return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
}
function ascii(b: Uint8Array, o: number, n: number): string {
  return String.fromCharCode(...b.subarray(o, o + n));
}

export function parseWav(bytes: Uint8Array): WavInfo {
  if (bytes.length < 12 || ascii(bytes, 0, 4) !== "RIFF" || ascii(bytes, 8, 4) !== "WAVE") {
    throw new Error("not a WAV file");
  }
  let fmt: Omit<WavInfo, "data" | "totalSamples"> | null = null;
  let totalSamples: number | null = null;
  let data: Uint8Array | null = null;
  let o = 12;
  while (o + 8 <= bytes.length) {
    const id = ascii(bytes, o, 4);
    const size = u32(bytes, o + 4);
    const body = o + 8;
    const end = Math.min(bytes.length, body + size);
    if (id === "fmt ") {
      const format = u16(bytes, body);
      const extra = size >= 20 ? u16(bytes, body + 16) : 0;
      fmt = {
        format,
        channels: u16(bytes, body + 2),
        sampleRate: u32(bytes, body + 4),
        blockAlign: u16(bytes, body + 12),
        bitsPerSample: u16(bytes, body + 14),
        samplesPerBlock: format === WAVE_FORMAT_IMA_ADPCM && extra >= 2 ? u16(bytes, body + 18) : 0,
      };
    } else if (id === "fact" && size >= 4) {
      totalSamples = u32(bytes, body);
    } else if (id === "data") {
      data = bytes.subarray(body, end); // tolerate a data size larger than the file (streamed WAVs)
      break;
    }
    o = body + size + (size & 1);
  }
  if (!fmt || !data) throw new Error("WAV without fmt or data chunk");
  return { ...fmt, totalSamples, data };
}

/** Decodes a mono PCM16 or IMA ADPCM WAV to 16-bit samples. */
export function decodeWav(bytes: Uint8Array): PcmAudio {
  const w = parseWav(bytes);
  if (w.channels !== 1) throw new Error(`expected mono audio, got ${w.channels} channels`);
  if (w.format === WAVE_FORMAT_PCM && w.bitsPerSample === 16) {
    const n = Math.floor(w.data.length / 2);
    const samples = new Int16Array(n);
    for (let i = 0; i < n; i++) samples[i] = (w.data[2 * i] | (w.data[2 * i + 1] << 8)) << 16 >> 16;
    return { sampleRate: w.sampleRate, samples };
  }
  if (w.format === WAVE_FORMAT_IMA_ADPCM && w.bitsPerSample === 4) {
    return {
      sampleRate: w.sampleRate,
      samples: imaDecode(w.data, w.blockAlign, w.samplesPerBlock || (w.blockAlign - 4) * 2 + 1, w.totalSamples),
    };
  }
  throw new Error(`unsupported WAV format ${w.format}/${w.bitsPerSample} bit`);
}

export function imaDecode(data: Uint8Array, blockAlign: number, samplesPerBlock: number, total: number | null): Int16Array {
  const blocks = Math.ceil(data.length / blockAlign);
  let capacity = 0;
  for (let b = 0; b < blocks; b++) {
    const len = Math.min(blockAlign, data.length - b * blockAlign);
    if (len >= 4) capacity += Math.min(samplesPerBlock, 1 + (len - 4) * 2);
  }
  const n = total === null ? capacity : Math.min(total, capacity);
  const out = new Int16Array(n);
  let k = 0;
  for (let b = 0; b < blocks && k < n; b++) {
    const base = b * blockAlign;
    const len = Math.min(blockAlign, data.length - base);
    if (len < 4) break;
    let predictor = (data[base] | (data[base + 1] << 8)) << 16 >> 16;
    let index = Math.min(88, data[base + 2]);
    out[k++] = predictor;
    const inBlock = Math.min(samplesPerBlock, 1 + (len - 4) * 2);
    for (let i = 1; i < inBlock && k < n; i++) {
      const byte = data[base + 4 + ((i - 1) >> 1)];
      const nibble = (i - 1) & 1 ? byte >> 4 : byte & 0x0f;
      const step = STEP_TABLE[index];
      let delta = step >> 3;
      if (nibble & 4) delta += step;
      if (nibble & 2) delta += step >> 1;
      if (nibble & 1) delta += step >> 2;
      predictor += nibble & 8 ? -delta : delta;
      if (predictor > 32767) predictor = 32767;
      else if (predictor < -32768) predictor = -32768;
      index += INDEX_TABLE[nibble];
      if (index < 0) index = 0;
      else if (index > 88) index = 88;
      out[k++] = predictor;
    }
  }
  return out;
}

/** Encodes mono 16-bit samples as IMA ADPCM blocks (blockAlign bytes each; the last may be short). */
export function imaEncode(samples: Int16Array, blockAlign = ADPCM_BLOCK_ALIGN): Uint8Array {
  const spb = (blockAlign - 4) * 2 + 1;
  const blocks = Math.ceil(samples.length / spb);
  const out = new Uint8Array(blocks * blockAlign);
  let length = 0;
  let index = 0;
  for (let b = 0; b < blocks; b++) {
    const start = b * spb;
    const count = Math.min(spb, samples.length - start);
    const base = b * blockAlign;
    let predictor = samples[start];
    out[base] = predictor & 0xff;
    out[base + 1] = (predictor >> 8) & 0xff;
    out[base + 2] = index;
    out[base + 3] = 0;
    for (let i = 1; i < count; i++) {
      let diff = samples[start + i] - predictor;
      let nibble = 0;
      if (diff < 0) {
        nibble = 8;
        diff = -diff;
      }
      let step = STEP_TABLE[index];
      let delta = step >> 3;
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
      predictor += nibble & 8 ? -delta : delta;
      if (predictor > 32767) predictor = 32767;
      else if (predictor < -32768) predictor = -32768;
      index += INDEX_TABLE[nibble];
      if (index < 0) index = 0;
      else if (index > 88) index = 88;
      const pos = base + 4 + ((i - 1) >> 1);
      out[pos] |= (i - 1) & 1 ? nibble << 4 : nibble;
    }
    length = base + 4 + Math.ceil((count - 1) / 2);
  }
  return out.subarray(0, length);
}

function writeAscii(b: Uint8Array, o: number, s: string) {
  for (let i = 0; i < s.length; i++) b[o + i] = s.charCodeAt(i);
}
function put16(b: Uint8Array, o: number, v: number) {
  b[o] = v & 0xff;
  b[o + 1] = (v >> 8) & 0xff;
}
function put32(b: Uint8Array, o: number, v: number) {
  b[o] = v & 0xff;
  b[o + 1] = (v >>> 8) & 0xff;
  b[o + 2] = (v >>> 16) & 0xff;
  b[o + 3] = (v >>> 24) & 0xff;
}

/** 16 kHz (or any rate) mono IMA ADPCM WAV: RIFF + fmt(20) + fact(4) + data = 60-byte header. */
export function encodeAdpcmWav(audio: PcmAudio, blockAlign = ADPCM_BLOCK_ALIGN): Uint8Array {
  const data = imaEncode(audio.samples, blockAlign);
  const spb = (blockAlign - 4) * 2 + 1;
  const out = new Uint8Array(60 + data.length);
  writeAscii(out, 0, "RIFF");
  put32(out, 4, 52 + data.length);
  writeAscii(out, 8, "WAVE");
  writeAscii(out, 12, "fmt ");
  put32(out, 16, 20);
  put16(out, 20, WAVE_FORMAT_IMA_ADPCM);
  put16(out, 22, 1);
  put32(out, 24, audio.sampleRate);
  put32(out, 28, Math.floor((audio.sampleRate * blockAlign) / spb));
  put16(out, 32, blockAlign);
  put16(out, 34, 4);
  put16(out, 36, 2);
  put16(out, 38, spb);
  writeAscii(out, 40, "fact");
  put32(out, 44, 4);
  put32(out, 48, audio.samples.length);
  writeAscii(out, 52, "data");
  put32(out, 56, data.length);
  out.set(data, 60);
  return out;
}

export function encodePcmWav(audio: PcmAudio): Uint8Array {
  const n = audio.samples.length;
  const out = new Uint8Array(44 + n * 2);
  writeAscii(out, 0, "RIFF");
  put32(out, 4, 36 + n * 2);
  writeAscii(out, 8, "WAVE");
  writeAscii(out, 12, "fmt ");
  put32(out, 16, 16);
  put16(out, 20, WAVE_FORMAT_PCM);
  put16(out, 22, 1);
  put32(out, 24, audio.sampleRate);
  put32(out, 28, audio.sampleRate * 2);
  put16(out, 32, 2);
  put16(out, 34, 16);
  writeAscii(out, 36, "data");
  put32(out, 40, n * 2);
  for (let i = 0; i < n; i++) put16(out, 44 + 2 * i, audio.samples[i]);
  return out;
}

/** Raw little-endian 16-bit PCM bytes (e.g. Gemini TTS audio/l16) to samples. */
export function pcm16FromBytes(bytes: Uint8Array): Int16Array {
  const n = Math.floor(bytes.length / 2);
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) out[i] = (bytes[2 * i] | (bytes[2 * i + 1] << 8)) << 16 >> 16;
  return out;
}

export function base64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function durationMs(audio: PcmAudio): number {
  return Math.round((audio.samples.length * 1000) / audio.sampleRate);
}

/** Samples [fromMs, toMs) of a clip (clamped). */
export function sliceMs(audio: PcmAudio, fromMs: number, toMs: number): PcmAudio {
  const a = Math.max(0, Math.floor((fromMs * audio.sampleRate) / 1000));
  const b = Math.min(audio.samples.length, Math.ceil((toMs * audio.sampleRate) / 1000));
  return { sampleRate: audio.sampleRate, samples: audio.samples.slice(a, Math.max(a, b)) };
}

export function concat(parts: PcmAudio[]): PcmAudio {
  if (parts.length === 0) return { sampleRate: 16000, samples: new Int16Array(0) };
  const rate = parts[0].sampleRate;
  for (const p of parts) if (p.sampleRate !== rate) throw new Error("cannot concatenate different sample rates");
  const out = new Int16Array(parts.reduce((n, p) => n + p.samples.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p.samples, o);
    o += p.samples.length;
  }
  return { sampleRate: rate, samples: out };
}
