// Run: deno test supabase/functions/_shared/
import { assert, assertEquals, assertThrows } from "jsr:@std/assert@1.0.13";
import {
  ADPCM_BLOCK_ALIGN,
  ADPCM_SAMPLES_PER_BLOCK,
  concat,
  decodeWav,
  encodeAdpcmWav,
  encodePcmWav,
  imaDecode,
  imaEncode,
  parseWav,
  sliceMs,
  WAVE_FORMAT_IMA_ADPCM,
} from "./audio.ts";

function speechLike(n: number, rate = 16000): Int16Array {
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / rate;
    const env = 0.5 + 0.5 * Math.sin(2 * Math.PI * 3 * t); // syllable-rate envelope
    out[i] = Math.round(env * (6000 * Math.sin(2 * Math.PI * 180 * t) + 2500 * Math.sin(2 * Math.PI * 1250 * t)));
  }
  return out;
}

function snrDb(ref: Int16Array, got: Int16Array): number {
  let s = 0, e = 0;
  for (let i = 0; i < ref.length; i++) {
    s += ref[i] * ref[i];
    e += (ref[i] - got[i]) ** 2;
  }
  return 10 * Math.log10(s / Math.max(1, e));
}

Deno.test("PCM WAV round trip is exact", () => {
  const samples = speechLike(16000);
  const back = decodeWav(encodePcmWav({ sampleRate: 16000, samples }));
  assertEquals(back.sampleRate, 16000);
  assertEquals(back.samples, samples);
});

Deno.test("ADPCM WAV: standard header, 4:1 size, exact sample count, good SNR", () => {
  const samples = speechLike(80000); // one 5 s chunk
  const wav = encodeAdpcmWav({ sampleRate: 16000, samples });
  const info = parseWav(wav);
  assertEquals(info.format, WAVE_FORMAT_IMA_ADPCM);
  assertEquals(info.blockAlign, ADPCM_BLOCK_ALIGN);
  assertEquals(info.samplesPerBlock, ADPCM_SAMPLES_PER_BLOCK);
  assertEquals(info.totalSamples, 80000);
  assert(wav.length < 160044 / 3.9, `size ${wav.length}`); // ~40 KB vs 160 KB PCM
  const back = decodeWav(wav);
  assertEquals(back.samples.length, 80000);
  const snr = snrDb(samples, back.samples);
  assert(snr > 20, `SNR ${snr.toFixed(1)} dB`);
});

Deno.test("ADPCM handles a short final block and tiny inputs", () => {
  for (const n of [1, 2, 3, ADPCM_SAMPLES_PER_BLOCK, ADPCM_SAMPLES_PER_BLOCK + 1, 2 * ADPCM_SAMPLES_PER_BLOCK + 7]) {
    const samples = speechLike(n);
    const data = imaEncode(samples);
    const back = imaDecode(data, ADPCM_BLOCK_ALIGN, ADPCM_SAMPLES_PER_BLOCK, n);
    assertEquals(back.length, n, `n=${n}`);
    assertEquals(back[0], samples[0]); // block header carries the first sample exactly
  }
});

Deno.test("ADPCM survives full-scale and silent input", () => {
  const loud = new Int16Array(4000);
  for (let i = 0; i < loud.length; i++) loud[i] = i % 40 < 20 ? 32767 : -32768;
  const back = decodeWav(encodeAdpcmWav({ sampleRate: 16000, samples: loud })).samples;
  for (const v of back) assert(v >= -32768 && v <= 32767);
  const silent = decodeWav(encodeAdpcmWav({ sampleRate: 16000, samples: new Int16Array(3000) })).samples;
  assert(silent.every((v) => Math.abs(v) < 8));
});

Deno.test("non-WAV and stereo input are rejected", () => {
  assertThrows(() => decodeWav(new Uint8Array([1, 2, 3, 4])));
  const wav = encodePcmWav({ sampleRate: 16000, samples: new Int16Array(10) });
  wav[22] = 2; // channels
  assertThrows(() => decodeWav(wav));
});

Deno.test("slice and concat", () => {
  const a = { sampleRate: 16000, samples: speechLike(16000) };
  const s = sliceMs(a, 250, 750);
  assertEquals(s.samples.length, 8000);
  assertEquals(s.samples[0], a.samples[4000]);
  assertEquals(concat([s, s]).samples.length, 16000);
  assertEquals(sliceMs(a, 900, 5000).samples.length, 1600);
});

Deno.test("student page decoder (data/adpcm.js) matches this decoder sample for sample", async () => {
  await import("../../../firmware/esp32_hub/data/adpcm.js");
  // deno-lint-ignore no-explicit-any
  const page = (globalThis as any).ElingoAdpcm;
  const samples = speechLike(2 * ADPCM_SAMPLES_PER_BLOCK + 333);
  const wav = encodeAdpcmWav({ sampleRate: 16000, samples });
  const ours = decodeWav(wav).samples;
  const theirs = page.decode(wav.slice().buffer);
  assertEquals(theirs.sampleRate, 16000);
  assertEquals(theirs.samples.length, ours.length);
  for (let i = 0; i < ours.length; i++) assertEquals(Math.round(theirs.samples[i] * 32768), ours[i]);
  assertEquals(page.decode(encodePcmWav({ sampleRate: 16000, samples }).buffer), null); // PCM: browser decodes
});
