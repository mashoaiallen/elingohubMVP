// Provider clients: Deepgram (speech-to-text) and Gemini (translation, text-to-speech).
// Keys come only from Edge Function secrets (DEEPGRAM_API_KEY, GEMINI_API_KEY); nothing here
// ever reaches the Hub or the student page. `fetch` is injectable so tests use mocked responses.
//
// Verified against the official docs on 2026-11-12:
//   Deepgram  POST https://api.deepgram.com/v1/listen, "Authorization: Token <key>", binary body,
//             response results.channels[0].alternatives[0].{transcript,confidence,words[]}.
//   Gemini    Interactions API (recommended): POST /v1beta/interactions, "x-goog-api-key",
//             { model, system_instruction, input, generation_config, store }, output in
//             steps[] (last "model_output" step). generateContent is still supported ("legacy")
//             and kept as a fallback (GEMINI_API=generate_content).
//             TTS: response_format { type: "audio", mime_type: "audio/l16", sample_rate },
//             base64 PCM in steps[].content[].data. 25 audio tokens per second.
//   We send store:false so the Interactions API does not keep lecture text (default: stored,
//   55 days on the paid tier).

import type { Word } from "./assembler.ts";
import { base64ToBytes, decodeWav, pcm16FromBytes, type PcmAudio } from "./audio.ts";

export type FetchLike = (input: string, init?: RequestInit) => Promise<Response>;

export class ProviderError extends Error {
  constructor(
    readonly provider: string,
    message: string,
    readonly retryable: boolean,
    readonly status = 0,
    readonly requestId: string | null = null,
    readonly retryAfterSec: number | null = null, // provider's own hint (Retry-After, "retry in Ns")
  ) {
    super(`${provider}: ${message}`);
  }
}

/** Retry hint from a Retry-After header or a "retry in 32s" / "retryDelay": "32s" body. */
export function retryAfterSec(res: Response, body: string): number | null {
  const h = res.headers.get("retry-after");
  if (h && /^\d+$/.test(h.trim())) return Number(h.trim());
  const m = /retry in (\d+(?:\.\d+)?)\s*s/i.exec(body) ?? /"retryDelay"\s*:\s*"(\d+(?:\.\d+)?)s"/.exec(body);
  return m ? Math.ceil(Number(m[1])) : null;
}

async function request(provider: string, f: FetchLike, url: string, init: RequestInit, timeoutMs: number): Promise<Response> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    return await f(url, { ...init, signal: ctrl.signal });
  } catch (e) {
    const timedOut = ctrl.signal.aborted;
    throw new ProviderError(provider, timedOut ? `timeout after ${timeoutMs} ms` : `network error: ${(e as Error).message}`, true);
  } finally {
    clearTimeout(timer);
  }
}

async function failure(provider: string, res: Response, requestId: string | null): Promise<ProviderError> {
  let body = "";
  try {
    body = await res.text();
  } catch { /* ignore */ }
  // 408/429/5xx are transient; other 4xx (bad key, bad audio, bad request) will not improve on retry.
  const retryable = res.status === 408 || res.status === 429 || res.status >= 500;
  return new ProviderError(provider, `HTTP ${res.status} ${body.slice(0, 300)}`.trim(), retryable, res.status, requestId, retryAfterSec(res, body));
}

// ------------------------------------------------------------------------------------ Deepgram

export interface TranscribeOptions {
  apiKey: string;
  wav: Uint8Array; // PCM16 WAV
  model?: string;
  language?: string; // "multi" (code-switching) or a BCP-47 code
  keyterms?: string[];
  timeoutMs?: number;
  fetch?: FetchLike;
}

export interface Transcription {
  transcript: string;
  confidence: number | null;
  words: Word[];
  language: string | null;
  requestId: string | null;
  durationSec: number;
}

export async function deepgramTranscribe(o: TranscribeOptions): Promise<Transcription> {
  const params = new URLSearchParams({
    model: o.model ?? "nova-3",
    language: o.language ?? "multi",
    smart_format: "true",
    punctuate: "true",
  });
  for (const k of o.keyterms ?? []) params.append("keyterm", k);
  const res = await request("deepgram", o.fetch ?? fetch, `https://api.deepgram.com/v1/listen?${params}`, {
    method: "POST",
    headers: { Authorization: `Token ${o.apiKey}`, "Content-Type": "audio/wav" },
    body: o.wav as Uint8Array<ArrayBuffer>,
  }, o.timeoutMs ?? 20_000);
  const requestId = res.headers.get("dg-request-id");
  if (!res.ok) throw await failure("deepgram", res, requestId);
  // deno-lint-ignore no-explicit-any
  let body: any;
  try {
    body = await res.json();
  } catch {
    throw new ProviderError("deepgram", "invalid JSON response", true, res.status, requestId);
  }
  const channel = body?.results?.channels?.[0];
  const alt = channel?.alternatives?.[0];
  if (!alt) throw new ProviderError("deepgram", "response has no alternatives", false, res.status, requestId);
  // deno-lint-ignore no-explicit-any
  const words: Word[] = (alt.words ?? []).map((w: any) => ({
    text: String(w.punctuated_word ?? w.word ?? ""),
    start: Number(w.start) || 0,
    end: Number(w.end) || 0,
    confidence: typeof w.confidence === "number" ? w.confidence : undefined,
    language: typeof w.language === "string" ? w.language : undefined,
  }));
  return {
    transcript: String(alt.transcript ?? ""),
    confidence: typeof alt.confidence === "number" ? alt.confidence : null,
    words,
    language: channel?.detected_language ?? alt?.languages?.[0] ?? null,
    requestId: body?.metadata?.request_id ?? requestId,
    durationSec: Number(body?.metadata?.duration) || 0,
  };
}

// -------------------------------------------------------------------------------------- Gemini

export type GeminiApi = "interactions" | "generate_content";
const GEMINI_BASE = "https://generativelanguage.googleapis.com/v1beta";

export interface GeminiUsage {
  inputTokens: number;
  outputTokens: number;
  thoughtTokens: number;
}

export interface TranslateOptions {
  apiKey: string;
  model: string;
  api?: GeminiApi;
  sourceLanguage: string | null; // e.g. "English"; null = detect
  targetLanguage: { code: string; name: string };
  text: string;
  context: { source: string; target: string | null }[]; // previous utterances, oldest first
  glossary?: string[];
  thinkingLevel?: string;
  timeoutMs?: number;
  fetch?: FetchLike;
}

export function translationPrompt(o: Pick<TranslateOptions, "sourceLanguage" | "targetLanguage" | "glossary">): string {
  const from = o.sourceLanguage ? `from ${o.sourceLanguage} ` : "";
  return [
    `You are the live interpreter for a university lecture. Translate the lecturer's words ${from}into ${o.targetLanguage.name} (${o.targetLanguage.code}).`,
    "Rules:",
    "- Translate everything that was said, completely and faithfully. Do not summarise, shorten, explain, add or omit anything.",
    "- Keep technical and academic terms accurate. Keep a term in its original form when the target language has no established equivalent.",
    "- Keep numbers, units, formulas, names and code exactly.",
    "- The text is speech recognised in short segments and may contain small recognition errors or begin mid-sentence; translate it as naturally as possible without inventing content.",
    "- The earlier segments are context only. Translate only the NEW SEGMENT, and never repeat text that was already translated.",
    "- Output only the translation: plain text, no quotes, labels, notes or markdown.",
    ...(o.glossary?.length ? [`- Use these course terms consistently: ${o.glossary.join("; ")}`] : []),
  ].join("\n");
}

export function translationInput(o: Pick<TranslateOptions, "text" | "context">): string {
  const ctx = o.context
    .filter((c) => c.source.trim())
    .map((c) => `Lecturer: ${c.source}${c.target ? `\nTranslation: ${c.target}` : ""}`)
    .join("\n\n");
  return `${ctx ? `EARLIER SEGMENTS (context, already translated):\n${ctx}\n\n` : ""}NEW SEGMENT:\n${o.text}`;
}

// deno-lint-ignore no-explicit-any
function interactionOutputs(body: any): any[] {
  const steps = Array.isArray(body?.steps) ? body.steps : [];
  // deno-lint-ignore no-explicit-any
  const outputs = steps.filter((s: any) => s?.type === "model_output");
  // deno-lint-ignore no-explicit-any
  const items: any[] = (outputs.length ? outputs : steps).flatMap((s: any) => (Array.isArray(s?.content) ? s.content : []));
  if (Array.isArray(body?.outputs)) items.push(...body.outputs);
  return items;
}

// deno-lint-ignore no-explicit-any
function usageOf(body: any): GeminiUsage {
  const u = body?.usage ?? {};
  const m = body?.usageMetadata ?? {};
  return {
    inputTokens: Number(u.total_input_tokens ?? m.promptTokenCount ?? 0) || 0,
    outputTokens: Number(u.total_output_tokens ?? m.candidatesTokenCount ?? 0) || 0,
    thoughtTokens: Number(u.total_thought_tokens ?? m.thoughtsTokenCount ?? 0) || 0,
  };
}

async function geminiCall(o: { apiKey: string; api: GeminiApi; model: string; timeoutMs: number; fetch?: FetchLike }, body: unknown) {
  const url = o.api === "interactions" ? `${GEMINI_BASE}/interactions` : `${GEMINI_BASE}/models/${o.model}:generateContent`;
  const res = await request("gemini", o.fetch ?? fetch, url, {
    method: "POST",
    headers: { "x-goog-api-key": o.apiKey, "Content-Type": "application/json" },
    body: JSON.stringify(body),
  }, o.timeoutMs);
  if (!res.ok) throw await failure("gemini", res, null);
  try {
    return await res.json();
  } catch {
    throw new ProviderError("gemini", "invalid JSON response", true, res.status);
  }
}

export interface Translation {
  text: string;
  usage: GeminiUsage;
  requestId: string | null;
}

export async function geminiTranslate(o: TranslateOptions): Promise<Translation> {
  const api = o.api ?? "interactions";
  const system = translationPrompt(o);
  const input = translationInput(o);
  const thinking = o.thinkingLevel ?? "low";
  const body = api === "interactions"
    ? {
      model: o.model,
      system_instruction: system,
      input,
      store: false,
      generation_config: { thinking_level: thinking, max_output_tokens: 2048 },
    }
    : {
      systemInstruction: { parts: [{ text: system }] },
      contents: [{ role: "user", parts: [{ text: input }] }],
      generationConfig: { thinkingConfig: { thinkingLevel: thinking }, maxOutputTokens: 2048 },
    };
  const res = await geminiCall({ ...o, api, timeoutMs: o.timeoutMs ?? 30_000 }, body);
  let text = "";
  if (api === "interactions") {
    text = interactionOutputs(res)
      // deno-lint-ignore no-explicit-any
      .filter((c: any) => typeof c?.text === "string" && (c.type === undefined || c.type === "text"))
      // deno-lint-ignore no-explicit-any
      .map((c: any) => c.text)
      .join("");
  } else {
    // deno-lint-ignore no-explicit-any
    text = (res?.candidates?.[0]?.content?.parts ?? []).filter((p: any) => !p.thought && typeof p.text === "string")
      // deno-lint-ignore no-explicit-any
      .map((p: any) => p.text).join("");
  }
  text = text.trim();
  if (!text) {
    const reason = res?.candidates?.[0]?.finishReason ?? res?.status ?? "empty";
    throw new ProviderError("gemini", `translation returned no text (${reason})`, reason !== "SAFETY");
  }
  return { text, usage: usageOf(res), requestId: res?.id ?? res?.responseId ?? null };
}

export interface SynthesizeOptions {
  apiKey: string;
  model: string;
  api?: GeminiApi;
  voice: string;
  text: string;
  style?: string;
  sampleRate?: number;
  timeoutMs?: number;
  fetch?: FetchLike;
}

export interface Speech {
  audio: PcmAudio;
  usage: GeminiUsage;
  requestId: string | null;
}

export const DEFAULT_TTS_STYLE = "a clear, calm university lecturer speaking at a steady, natural pace";

export async function geminiSynthesize(o: SynthesizeOptions): Promise<Speech> {
  const api = o.api ?? "interactions";
  const rate = o.sampleRate ?? 16000;
  const style = o.style ?? DEFAULT_TTS_STYLE;
  const body = api === "interactions"
    ? {
      model: o.model,
      input: [{ type: "user_input", content: [{ type: "text", text: o.text, annotations: [{ type: "speech_metadata", style }] }] }],
      store: false,
      response_format: { type: "audio", mime_type: "audio/l16", sample_rate: rate },
      generation_config: { speech_config: [{ voice: o.voice }] },
    }
    : {
      contents: [{ role: "user", parts: [{ text: `Read aloud as ${style}: ${o.text}` }] }],
      generationConfig: {
        responseModalities: ["AUDIO"],
        speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: o.voice } } },
      },
    };
  const res = await geminiCall({ ...o, api, timeoutMs: o.timeoutMs ?? 45_000 }, body);
  // deno-lint-ignore no-explicit-any
  let part: { data: string; mime: string } | null = null;
  if (api === "interactions") {
    // deno-lint-ignore no-explicit-any
    const item = interactionOutputs(res).find((c: any) => typeof c?.data === "string");
    if (item) part = { data: item.data, mime: String(item.mime_type ?? item.mimeType ?? "audio/l16") };
  } else {
    // deno-lint-ignore no-explicit-any
    const p = (res?.candidates?.[0]?.content?.parts ?? []).find((x: any) => x?.inlineData?.data);
    if (p) part = { data: p.inlineData.data, mime: String(p.inlineData.mimeType ?? "audio/L16;rate=24000") };
  }
  if (!part) throw new ProviderError("gemini", "speech response had no audio", true);
  return { audio: decodeSpeech(base64ToBytes(part.data), part.mime, api === "interactions" ? rate : 24000), usage: usageOf(res), requestId: res?.id ?? null };
}

/** TTS audio arrives as WAV (unary default) or headerless L16; the rate is in the WAV or the mime type. */
export function decodeSpeech(bytes: Uint8Array, mime: string, defaultRate: number): PcmAudio {
  if (bytes.length >= 12 && String.fromCharCode(...bytes.subarray(0, 4)) === "RIFF") return decodeWav(bytes);
  const m = /rate=(\d+)/i.exec(mime);
  return { sampleRate: m ? Number(m[1]) : defaultRate, samples: pcm16FromBytes(bytes) };
}

/** Billable audio tokens for Gemini TTS output (25 tokens per second, per the pricing page). */
export function ttsAudioTokens(durationMs: number): number {
  return Math.ceil((durationMs / 1000) * 25);
}
