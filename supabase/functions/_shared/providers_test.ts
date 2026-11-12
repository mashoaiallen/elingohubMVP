import { assert, assertEquals, assertRejects } from "jsr:@std/assert@1.0.13";
import { encodePcmWav } from "./audio.ts";
import {
  decodeSpeech,
  deepgramTranscribe,
  type FetchLike,
  geminiSynthesize,
  geminiTranslate,
  ProviderError,
  translationInput,
  ttsAudioTokens,
} from "./providers.ts";

const json = (v: unknown, status = 200) => new Response(JSON.stringify(v), { status });
const wav = encodePcmWav({ sampleRate: 16000, samples: new Int16Array(1600) });

Deno.test("Deepgram: words use punctuated_word; language and request id are read", async () => {
  const f: FetchLike = () =>
    Promise.resolve(json({
      metadata: { request_id: "r-1", duration: 0.1 },
      results: { channels: [{ detected_language: "af", alternatives: [{ transcript: "goeie môre", confidence: 0.9, words: [{ word: "goeie", punctuated_word: "Goeie", start: 0, end: 0.3 }, { word: "môre", punctuated_word: "môre.", start: 0.35, end: 0.7 }] }] }] },
    }));
  const t = await deepgramTranscribe({ apiKey: "k", wav, fetch: f });
  assertEquals(t.words.map((w) => w.text), ["Goeie", "môre."]);
  assertEquals([t.language, t.requestId, t.confidence], ["af", "r-1", 0.9]);
});

Deno.test("timeouts and 5xx/429 are retryable; other 4xx are not", async () => {
  const hang: FetchLike = (_u, init) =>
    new Promise((_r, reject) => init?.signal?.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError"))));
  const e1 = await assertRejects(() => deepgramTranscribe({ apiKey: "k", wav, fetch: hang, timeoutMs: 30 }), ProviderError);
  assert(e1.retryable && /timeout/.test(e1.message));
  for (const [status, retryable] of [[429, true], [500, true], [503, true], [400, false], [401, false], [403, false]] as const) {
    const e = await assertRejects(() => deepgramTranscribe({ apiKey: "k", wav, fetch: () => Promise.resolve(new Response("x", { status })) }), ProviderError);
    assertEquals(e.retryable, retryable, `HTTP ${status}`);
  }
});

Deno.test("Gemini translate: Interactions output text, thought steps ignored, usage read", async () => {
  const f: FetchLike = (url) => {
    assert(url.endsWith("/v1beta/interactions"));
    return Promise.resolve(json({ steps: [{ type: "thought", summary: [{ type: "text", text: "thinking..." }] }, { type: "model_output", content: [{ type: "text", text: " Energie word behou. " }] }], usage: { total_input_tokens: 10, total_output_tokens: 5, total_thought_tokens: 7 } }));
  };
  const t = await geminiTranslate({ apiKey: "k", model: "gemini-3.8-flash", sourceLanguage: "English", targetLanguage: { code: "af", name: "Afrikaans" }, text: "Energy is conserved.", context: [], fetch: f });
  assertEquals(t.text, "Energie word behou.");
  assertEquals(t.usage, { inputTokens: 10, outputTokens: 5, thoughtTokens: 7 });
});

Deno.test("Gemini translate: generateContent fallback shape", async () => {
  const f: FetchLike = (url, init) => {
    assert(url.includes("/models/gemini-3.8-flash:generateContent"));
    const b = JSON.parse(String(init?.body));
    assertEquals(b.generationConfig.thinkingConfig.thinkingLevel, "low");
    return Promise.resolve(json({ candidates: [{ content: { parts: [{ text: "plan", thought: true }, { text: "Hallo" }] } }], usageMetadata: { promptTokenCount: 3, candidatesTokenCount: 1, thoughtsTokenCount: 2 } }));
  };
  const t = await geminiTranslate({ apiKey: "k", api: "generate_content", model: "gemini-3.8-flash", sourceLanguage: null, targetLanguage: { code: "af", name: "Afrikaans" }, text: "Hello", context: [], fetch: f });
  assertEquals([t.text, t.usage.thoughtTokens], ["Hallo", 2]);
});

Deno.test("Gemini translate: an empty answer is an error, not an empty segment", async () => {
  const f: FetchLike = () => Promise.resolve(json({ steps: [{ type: "model_output", content: [] }] }));
  await assertRejects(() => geminiTranslate({ apiKey: "k", model: "m", sourceLanguage: null, targetLanguage: { code: "af", name: "Afrikaans" }, text: "x", context: [], fetch: f }), ProviderError);
});

Deno.test("translation input keeps context separate from the new segment", () => {
  const s = translationInput({ text: "Second.", context: [{ source: "First.", target: "Eerste." }] });
  assert(s.indexOf("First.") < s.indexOf("NEW SEGMENT:\nSecond."));
  assertEquals(translationInput({ text: "Only.", context: [] }), "NEW SEGMENT:\nOnly.");
});

Deno.test("TTS audio: WAV and headerless L16 (rate from the mime type) both decode", () => {
  const w = decodeSpeech(encodePcmWav({ sampleRate: 24000, samples: new Int16Array(240) }), "audio/wav", 16000);
  assertEquals([w.sampleRate, w.samples.length], [24000, 240]);
  const raw = decodeSpeech(new Uint8Array(320), "audio/L16;codec=pcm;rate=24000", 16000);
  assertEquals([raw.sampleRate, raw.samples.length], [24000, 160]);
  assertEquals(decodeSpeech(new Uint8Array(320), "audio/l16", 16000).sampleRate, 16000);
  assertEquals(ttsAudioTokens(2000), 50);
});

Deno.test("TTS: request shape and missing audio", async () => {
  let body: Record<string, unknown> = {};
  const f: FetchLike = (_u, init) => {
    body = JSON.parse(String(init?.body));
    return Promise.resolve(json({ steps: [{ type: "model_output", content: [{ type: "text", text: "sorry" }] }] }));
  };
  await assertRejects(() => geminiSynthesize({ apiKey: "k", model: "gemini-3.8-flash-tts", voice: "Kore", text: "Hallo", fetch: f }), ProviderError);
  assertEquals(body.store, false);
  assertEquals((body.generation_config as { speech_config: unknown }).speech_config, [{ voice: "Kore" }]);
});
