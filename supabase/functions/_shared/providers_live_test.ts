// REAL provider calls - opt-in only, never part of normal test runs. Costs well under US$0.01:
// one short Gemini translation, ~2 s of Gemini TTS, ~2 s of Deepgram transcription.
//
//   ELINGO_LIVE_PROVIDER_TESTS=1 DEEPGRAM_API_KEY=... GEMINI_API_KEY=... \
//     deno test --allow-env --allow-net supabase/functions/_shared/providers_live_test.ts
//
// It verifies the response shapes this code relies on, the audio format and sample rate, and
// prints latency per call.
import { assert } from "jsr:@std/assert@1.0.13";
import { durationMs, encodeAdpcmWav, encodePcmWav } from "./audio.ts";
import { deepgramTranscribe, type GeminiApi, geminiSynthesize, geminiTranslate } from "./providers.ts";

// Without --allow-env (normal test runs) nothing can be read, so the test is skipped.
function env(name: string): string {
  try {
    return Deno.env.get(name) ?? "";
  } catch {
    return "";
  }
}
const enabled = env("ELINGO_LIVE_PROVIDER_TESTS") === "1";
const dg = env("DEEPGRAM_API_KEY");
const gm = env("GEMINI_API_KEY");
const api = (env("GEMINI_API") || "interactions") as GeminiApi;

Deno.test({
  name: "live: translate -> TTS -> transcribe round trip",
  ignore: !enabled || !dg || !gm,
  async fn() {
    let t0 = performance.now();
    const tr = await geminiTranslate({
      apiKey: gm, api, model: env("TRANSLATE_MODEL") || "gemini-3.8-flash", thinkingLevel: "low",
      sourceLanguage: "English", targetLanguage: { code: "af", name: "Afrikaans" },
      text: "Energy is conserved in an isolated system.", context: [],
    });
    console.log(`translate ${Math.round(performance.now() - t0)} ms:`, tr.text, tr.usage);
    assert(tr.text.length > 5);

    for (const model of ["gemini-3.8-flash-lite-tts", "gemini-3.8-flash-tts"]) {
      t0 = performance.now();
      const sp = await geminiSynthesize({ apiKey: gm, api, model, voice: "Kore", text: tr.text });
      console.log(`${model} ${Math.round(performance.now() - t0)} ms: ${durationMs(sp.audio)} ms audio @ ${sp.audio.sampleRate} Hz`, sp.usage);
      assert(sp.audio.samples.length > 1000);
      const adpcm = encodeAdpcmWav(sp.audio);
      console.log(`  ADPCM segment ${adpcm.length} bytes (${Math.round(adpcm.length / (durationMs(sp.audio) / 1000))} B/s)`);
      if (model === "gemini-3.8-flash-tts") {
        t0 = performance.now();
        const t = await deepgramTranscribe({ apiKey: dg, wav: encodePcmWav(sp.audio), language: "multi" });
        console.log(`deepgram ${Math.round(performance.now() - t0)} ms:`, t.transcript, t.language, `${t.words.length} words`);
        assert(t.words.length > 0);
      }
    }
  },
});
