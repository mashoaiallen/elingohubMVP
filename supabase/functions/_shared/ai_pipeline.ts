// Milestone 2 workers: STT stage (process-chunk) and translation + TTS stage (process-translation).
// All decisions that need consistency (order, demand, idempotency, cost cap) are taken in SQL
// (worker_stt_begin / worker_tts_begin, see migration 20261113090000); this module does the
// provider calls and the audio work, and reports outcomes back. Providers and the database are
// injected, so tests run with mocks and never touch a paid API.

import {
  baseLanguage,
  normalizeState,
  type PlacedWord,
  PREROLL_SEC,
  segment,
  skipChunk,
  stitch,
  type SttState,
  type Utterance,
} from "./assembler.ts";
import { concat, decodeWav, durationMs, encodeAdpcmWav, encodePcmWav, type PcmAudio, sliceMs } from "./audio.ts";
import {
  deepgramTranscribe,
  type FetchLike,
  geminiSynthesize,
  geminiTranslate,
  type GeminiApi,
  ProviderError,
  ttsAudioTokens,
} from "./providers.ts";

export const MAX_PROVIDER_ATTEMPTS = 3; // Deepgram
// Translation/TTS failures fall back to the lecturer's original audio at once; only our own
// storage upload is retried (once).
export const MAX_TTS_ATTEMPTS = 2;
export const WAIT_DELAY_SEC = 2;

export interface DbError {
  message: string;
  code?: string;
  statusCode?: string | number;
}

export interface StorageApi {
  download(path: string): PromiseLike<{ data: Blob | null; error: DbError | null }>;
  upload(
    path: string,
    body: Uint8Array,
    options?: { contentType?: string; upsert?: boolean },
  ): PromiseLike<{ data: unknown; error: DbError | null }>;
}

export interface AiDb {
  rpc(fn: string, args?: Record<string, unknown>): PromiseLike<{ data: unknown; error: DbError | null }>;
  storage: { from(bucket: string): StorageApi };
}

export interface Providers {
  deepgramKey: string;
  geminiKey: string;
  geminiApi?: GeminiApi;
  fetch?: FetchLike;
  now?: () => number;
}

async function call<T>(db: AiDb, fn: string, args: Record<string, unknown>): Promise<T> {
  const { data, error } = await db.rpc(fn, args);
  if (error) throw new Error(`${fn}: ${error.message}`);
  return data as T;
}

async function download(db: AiDb, bucket: string, path: string): Promise<Uint8Array> {
  const { data, error } = await db.storage.from(bucket).download(path);
  if (error || !data) throw new Error(`download ${bucket}/${path}: ${error?.message ?? "no data"}`);
  return new Uint8Array(await data.arrayBuffer());
}

// Exponential backoff (2, 4, 8 s), or the provider's own "retry after" hint when it gives one
// (rate limits), capped at 30 s so the audio is still worth delivering.
function backoffSec(attempt: number, hint: number | null = null): number {
  return Math.min(30, Math.max(2 ** attempt, hint ?? 0));
}

async function usage(db: AiDb, row: {
  sessionId: string;
  chunkId?: number | null;
  utteranceId?: number | null;
  language?: string | null;
  provider: string;
  operation: string;
  model: string;
  items: { unitType: string; units: number }[];
  status: "ok" | "error";
  latencyMs: number;
  attempt: number;
  requestId?: string | null;
  error?: string | null;
  meta?: Record<string, unknown>;
}) {
  // Usage tracking must never fail the work it measures.
  await db.rpc("worker_usage", {
    p_session_id: row.sessionId,
    p_chunk_id: row.chunkId ?? null,
    p_utterance_id: row.utteranceId ?? null,
    p_language: row.language ?? null,
    p_provider: row.provider,
    p_operation: row.operation,
    p_model: row.model,
    p_items: row.items,
    p_status: row.status,
    p_latency_ms: Math.round(row.latencyMs),
    p_attempt: row.attempt,
    p_request_id: row.requestId ?? null,
    p_error: row.error ?? null,
    p_meta: row.meta ?? {},
  });
}

async function log(db: AiDb, level: "info" | "warn" | "error", stage: string, message: string, chunkId: number | null, detail: Record<string, unknown> = {}, language: string | null = null) {
  await db.rpc("worker_log", { p_level: level, p_stage: stage, p_message: message, p_chunk_id: chunkId, p_language: language, p_detail: detail });
}

// ===================================================================================== STT stage

export interface SttMessage {
  msg_id: number;
  read_ct: number;
  chunk_id: number | null;
  flush_session: string | null;
}

interface SttBegin {
  action: "process" | "skip" | "wait" | "final";
  reason: string | null;
  chunk?: { id: number; sessionId: string; hubId: string; seq: number; path: string; durationMs: number; attempts: number };
  prev?: { seq: number; path: string; durationMs: number } | null;
  isLast?: boolean;
  late?: boolean;
  state?: unknown;
  languages?: string[];
  sttModel?: string;
  sttLanguage?: string;
}

export interface SttResult {
  msgId: number;
  outcome: string;
  utterances?: number;
  error?: string;
}

function trailingPause(words: PlacedWord[], seq: number, durationSec: number): number {
  const own = words.filter((w) => w.seq === seq);
  return own.length ? Math.max(0, durationSec - own[own.length - 1].end) : durationSec;
}

async function commitStt(db: AiDb, msg: SttMessage, chunkId: number, status: string, state: SttState | null, utterances: Utterance[], transcript: unknown = null, error: string | null = null) {
  return await call<{ utterances: number; jobs: number }>(db, "worker_stt_commit", {
    p_chunk_id: chunkId,
    p_msg_id: msg.msg_id,
    p_status: status,
    p_state: state,
    p_transcript: transcript,
    p_utterances: utterances,
    p_error: error,
  });
}

export async function processSttMessage(db: AiDb, msg: SttMessage, p: Providers): Promise<SttResult> {
  if (msg.flush_session) return await flushSession(db, msg);
  if (msg.chunk_id === null) {
    await call(db, "worker_queue_archive", { p_queue: "chunk_jobs", p_msg_id: msg.msg_id });
    return { msgId: msg.msg_id, outcome: "invalid" };
  }

  const b = await call<SttBegin>(db, "worker_stt_begin", { p_chunk_id: msg.chunk_id, p_lease_sec: 90 });
  if (b.action === "final") {
    await call(db, "worker_queue_archive", { p_queue: "chunk_jobs", p_msg_id: msg.msg_id });
    return { msgId: msg.msg_id, outcome: "already_final" };
  }
  if (b.action === "wait") {
    await call(db, "worker_queue_delay", { p_queue: "chunk_jobs", p_msg_id: msg.msg_id, p_delay_sec: WAIT_DELAY_SEC });
    return { msgId: msg.msg_id, outcome: `wait_${b.reason}` };
  }
  const chunk = b.chunk!;
  const state = normalizeState(b.state);

  if (b.action === "skip") {
    if (b.late) {
      await commitStt(db, msg, chunk.id, b.reason ?? "skipped_stale", null, []);
      return { msgId: msg.msg_id, outcome: b.reason ?? "skipped_stale" };
    }
    // Nothing is transcribed; a sentence in progress ends here (silence, no listeners, stale).
    const r = skipChunk(state, chunk.seq);
    const s = segment(r.state, r.committed, { currentSeq: chunk.seq, force: true, trailingPauseSec: 0, fallbackLanguage: null });
    const res = await commitStt(db, msg, chunk.id, b.reason ?? "skipped_no_listeners", s.state, s.utterances);
    return { msgId: msg.msg_id, outcome: b.reason ?? "skipped", utterances: res.utterances };
  }

  // ---- transcribe: this chunk, preceded by the end of the previous one when contiguous
  const model = b.sttModel ?? "nova-3";
  const language = b.sttLanguage ?? "multi";
  const rateModel = language === "multi" ? `${model}/multi` : model;
  let audio: PcmAudio;
  let prerollSec = 0;
  try {
    const own = decodeWav(await download(db, "source-audio", chunk.path));
    const parts: PcmAudio[] = [];
    if (b.prev && b.prev.seq === chunk.seq - 1 && state.prevSeq === b.prev.seq) {
      try {
        const prev = decodeWav(await download(db, "source-audio", b.prev.path));
        if (prev.sampleRate === own.sampleRate) {
          const d = durationMs(prev);
          const pre = sliceMs(prev, Math.max(0, d - PREROLL_SEC * 1000), d);
          prerollSec = pre.samples.length / pre.sampleRate;
          parts.push(pre);
        }
      } catch (e) {
        await log(db, "warn", "stt", `pre-roll unavailable: ${(e as Error).message}`, chunk.id);
      }
    }
    parts.push(own);
    audio = concat(parts);
  } catch (e) {
    // Unreadable audio will not improve on retry: end the chunk, keep the session going.
    const r = skipChunk(state, chunk.seq);
    const s = segment(r.state, r.committed, { currentSeq: chunk.seq, force: true, trailingPauseSec: 0, fallbackLanguage: null });
    await log(db, "error", "stt", `audio unreadable: ${(e as Error).message}`, chunk.id);
    await commitStt(db, msg, chunk.id, "failed", s.state, s.utterances, null, (e as Error).message);
    return { msgId: msg.msg_id, outcome: "failed", error: (e as Error).message };
  }

  const durationSec = chunk.durationMs / 1000;
  const t0 = performance.now();
  try {
    const t = await deepgramTranscribe({ apiKey: p.deepgramKey, wav: encodePcmWav(audio), model, language, fetch: p.fetch });
    const audioMinutes = audio.samples.length / audio.sampleRate / 60;
    await usage(db, {
      sessionId: chunk.sessionId, chunkId: chunk.id, provider: "deepgram", operation: "transcribe", model: rateModel,
      items: [{ unitType: "audio_minutes", units: audioMinutes }], status: "ok", latencyMs: performance.now() - t0,
      attempt: chunk.attempts, requestId: t.requestId, meta: { prerollSec, words: t.words.length },
    });
    const r = stitch(state, { seq: chunk.seq, durationSec, prerollSec, final: !!b.isLast }, t.words);
    const s = segment(r.state, r.committed, {
      currentSeq: chunk.seq,
      force: !!b.isLast,
      trailingPauseSec: trailingPause(r.committed.concat(r.state.held.map((w) => ({ ...w, seq: chunk.seq }))), chunk.seq, durationSec),
      fallbackLanguage: t.language ? baseLanguage(t.language) : null,
    });
    const res = await commitStt(db, msg, chunk.id, "done", s.state, s.utterances, {
      provider: "deepgram",
      model,
      text: t.transcript,
      confidence: t.confidence,
      language: t.language,
      words: t.words,
      prerollMs: Math.round(prerollSec * 1000),
      requestId: t.requestId,
      committed: r.committed.map((w) => w.text).join(" "),
    });
    return { msgId: msg.msg_id, outcome: "done", utterances: res.utterances };
  } catch (e) {
    const err = e instanceof ProviderError ? e : new ProviderError("deepgram", (e as Error).message, true);
    await usage(db, {
      sessionId: chunk.sessionId, chunkId: chunk.id, provider: "deepgram", operation: "transcribe", model: rateModel,
      items: [], status: "error", latencyMs: performance.now() - t0, attempt: chunk.attempts, requestId: err.requestId,
      error: err.message,
    });
    await log(db, "error", "stt", err.message, chunk.id, { attempt: chunk.attempts, retryable: err.retryable });
    if (err.retryable && chunk.attempts < MAX_PROVIDER_ATTEMPTS) {
      await call(db, "worker_stt_retry", { p_chunk_id: chunk.id, p_msg_id: msg.msg_id, p_delay_sec: backoffSec(chunk.attempts, err.retryAfterSec), p_error: err.message });
      return { msgId: msg.msg_id, outcome: "retry", error: err.message };
    }
    // Dead letter: the chunk is marked failed, the session moves on to the next chunk.
    const r = skipChunk(state, chunk.seq);
    const s = segment(r.state, r.committed, { currentSeq: chunk.seq, force: true, trailingPauseSec: 0, fallbackLanguage: null });
    await commitStt(db, msg, chunk.id, "failed", s.state, s.utterances, null, err.message);
    return { msgId: msg.msg_id, outcome: "failed", error: err.message };
  }
}

async function flushSession(db: AiDb, msg: SttMessage): Promise<SttResult> {
  const b = await call<{ action: string; state?: unknown; nextSeq?: number }>(db, "worker_stt_flush_begin", {
    p_session_id: msg.flush_session,
    p_lease_sec: 30,
  });
  if (b.action === "wait") {
    if (msg.read_ct > 60) { // ~2 minutes of waiting: give up on the tail sentence
      await call(db, "worker_stt_flush_commit", { p_session_id: msg.flush_session, p_msg_id: msg.msg_id, p_utterances: [] });
      return { msgId: msg.msg_id, outcome: "flush_abandoned" };
    }
    await call(db, "worker_queue_delay", { p_queue: "chunk_jobs", p_msg_id: msg.msg_id, p_delay_sec: WAIT_DELAY_SEC });
    return { msgId: msg.msg_id, outcome: "wait_flush" };
  }
  if (b.action !== "process") {
    await call(db, "worker_queue_archive", { p_queue: "chunk_jobs", p_msg_id: msg.msg_id });
    return { msgId: msg.msg_id, outcome: "already_flushed" };
  }
  const state = normalizeState(b.state);
  const r = skipChunk(state, b.nextSeq ?? (state.prevSeq ?? 0) + 1);
  const s = segment(r.state, r.committed, { currentSeq: b.nextSeq ?? 0, force: true, trailingPauseSec: 0, fallbackLanguage: null });
  const res = await call<{ utterances: number }>(db, "worker_stt_flush_commit", {
    p_session_id: msg.flush_session,
    p_msg_id: msg.msg_id,
    p_utterances: s.utterances,
  });
  return { msgId: msg.msg_id, outcome: "flushed", utterances: res.utterances };
}

export async function processSttBatch(db: AiDb, p: Providers, opts: { maxJobs: number; vtSec: number; deadlineMs: number }) {
  const started = Date.now();
  const results: SttResult[] = [];
  while (Date.now() - started < opts.deadlineMs) {
    const msgs = await call<SttMessage[] | null>(db, "worker_claim_stt", { p_qty: opts.maxJobs, p_vt: opts.vtSec });
    if (!msgs || msgs.length === 0) break;
    // Chunks of one session are serialised by the SQL lease; different sessions run in parallel.
    const batch = await Promise.all(msgs.map((m) =>
      processSttMessage(db, m, p).catch(async (e) => {
        await log(db, "error", "stt", (e as Error).message, m.chunk_id, { msgId: m.msg_id });
        return { msgId: m.msg_id, outcome: "error", error: (e as Error).message };
      })
    ));
    results.push(...batch);
    if (batch.every((r) => r.outcome.startsWith("wait"))) break; // nothing runnable right now
  }
  return { processed: results.length, results };
}

// ========================================================================== Translation + TTS

export interface TtsMessage {
  msg_id: number;
  read_ct: number;
  job_id: number;
}

interface TtsBegin {
  action: "process" | "skip" | "final";
  reason: string | null;
  method: "tts" | "passthrough" | "fallback";
  job: { id: number; attempts: number; translatedText: string | null; language: string };
  utterance: { id: number; seq: number; text: string | null; language: string | null; startSeq: number; startMs: number; endSeq: number; endMs: number };
  session: { id: string; hubId: string };
  language: { code: string; name: string; ttsModel: string | null; ttsVoice: string | null };
  sourceLanguageName: string | null;
  context: { source: string; target: string | null }[];
  chunks: { seq: number; path: string; durationMs: number }[];
  chunksComplete?: boolean;
  cooldowns?: { translate: string | null; tts: string | null };
  translateModel: string;
  translateThinking: string;
}

export interface TtsResult {
  msgId: number;
  outcome: string;
  error?: string;
}

export function segmentPath(hubId: string, sessionId: string, lang: string, utteranceSeq: number): string {
  return `${hubId}/${sessionId}/${lang}/u${String(utteranceSeq).padStart(6, "0")}.wav`;
}

const PASSTHROUGH_PAD_MS = 150;

async function lecturerAudio(db: AiDb, b: TtsBegin): Promise<PcmAudio> {
  const parts: PcmAudio[] = [];
  for (const c of b.chunks) {
    const a = decodeWav(await download(db, "source-audio", c.path));
    const from = c.seq === b.utterance.startSeq ? b.utterance.startMs - PASSTHROUGH_PAD_MS : 0;
    const to = c.seq === b.utterance.endSeq ? b.utterance.endMs + PASSTHROUGH_PAD_MS : durationMs(a);
    parts.push(sliceMs(a, from, to));
  }
  return concat(parts);
}

export async function processTtsMessage(db: AiDb, msg: TtsMessage, p: Providers): Promise<TtsResult> {
  const b = await call<TtsBegin>(db, "worker_tts_begin", { p_job_id: msg.job_id });
  const commit = (status: string, extra: Record<string, unknown> = {}) =>
    call(db, "worker_tts_commit", { p_job_id: msg.job_id, p_msg_id: msg.msg_id, p_status: status, ...extra });
  if (b.action === "final") {
    await call(db, "worker_queue_archive", { p_queue: "tts_jobs", p_msg_id: msg.msg_id });
    return { msgId: msg.msg_id, outcome: "already_final" };
  }
  if (b.action === "skip") {
    await commit(b.reason ?? "skipped_no_listeners");
    return { msgId: msg.msg_id, outcome: b.reason ?? "skipped" };
  }

  const lang = b.language.code;
  const base = { sessionId: b.session.id, utteranceId: b.utterance.id, language: lang, attempt: b.job.attempts };

  /** Stores audio for this job and marks it ready. */
  async function deliver(audio: PcmAudio, mode: "ai" | "passthrough" | "fallback", note: string | null = null) {
    if (audio.samples.length === 0) throw new ProviderError("audio", "no audio produced", false);
    const wav = encodeAdpcmWav(audio);
    const path = segmentPath(b.session.hubId, b.session.id, lang, b.utterance.seq);
    const { error } = await db.storage.from("translated-audio").upload(path, wav, { contentType: "audio/wav", upsert: true });
    if (error) throw new ProviderError("storage", error.message, true);
    await commit("ready", { p_path: path, p_bytes: wav.length, p_duration_ms: durationMs(audio), p_error: note, p_mode: mode });
  }

  /** The lecturer's own audio for this utterance: what every listener gets when Gemini fails. */
  async function fallback(reason: string): Promise<TtsResult> {
    try {
      if (!b.chunks.length) throw new Error("source audio no longer available");
      await deliver(await lecturerAudio(db, b), "fallback", `original audio: ${reason}`);
      await log(db, "warn", "fallback", `original audio delivered: ${reason}`, null, { jobId: b.job.id }, lang);
      return { msgId: msg.msg_id, outcome: "fallback", error: reason };
    } catch (e) {
      const m = `${reason}; fallback failed: ${(e as Error).message}`;
      await commit("failed", { p_error: m });
      return { msgId: msg.msg_id, outcome: "failed", error: m };
    }
  }

  if (b.method === "passthrough") {
    try {
      await deliver(await lecturerAudio(db, b), "passthrough");
      return { msgId: msg.msg_id, outcome: "ready" };
    } catch (e) {
      await commit("failed", { p_error: (e as Error).message });
      return { msgId: msg.msg_id, outcome: "failed", error: (e as Error).message };
    }
  }
  if (b.method === "fallback") return await fallback("session cost cap reached");

  const ttsModel = b.language.ttsModel ?? "gemini-3.8-flash-tts";
  const now = Date.now();
  const cooling = (until: string | null | undefined) => !!until && Date.parse(until) > now;
  // A rate-limited model is not called again until its cool-down ends: no wasted requests, and
  // listeners get the original audio straight away instead of waiting for retries.
  if (!b.job.translatedText && cooling(b.cooldowns?.translate)) return await fallback(`${b.translateModel} rate-limited`);
  if (cooling(b.cooldowns?.tts)) return await fallback(`${ttsModel} rate-limited`);

  let stage = "translate";
  let model = b.translateModel;
  try {
    let text = b.job.translatedText;
    if (!text) {
      const t0 = performance.now();
      try {
        const t = await geminiTranslate({
          apiKey: p.geminiKey,
          api: p.geminiApi,
          model: b.translateModel,
          thinkingLevel: b.translateThinking,
          sourceLanguage: b.sourceLanguageName,
          targetLanguage: { code: lang, name: b.language.name },
          text: b.utterance.text ?? "",
          context: b.context,
          fetch: p.fetch,
        });
        text = t.text;
        await usage(db, {
          ...base, provider: "gemini", operation: "translate", model: b.translateModel, status: "ok",
          items: [
            { unitType: "input_tokens", units: t.usage.inputTokens },
            { unitType: "output_tokens", units: t.usage.outputTokens + t.usage.thoughtTokens },
          ],
          latencyMs: performance.now() - t0, requestId: t.requestId,
          meta: { thoughtTokens: t.usage.thoughtTokens },
        });
      } catch (e) {
        await usage(db, { ...base, provider: "gemini", operation: "translate", model: b.translateModel, status: "error", items: [], latencyMs: performance.now() - t0, error: (e as Error).message });
        throw e;
      }
      await call(db, "worker_tts_save_translation", { p_job_id: b.job.id, p_text: text });
    }
    stage = "tts";
    model = ttsModel;
    const t0 = performance.now();
    let audio: PcmAudio;
    try {
      const sp = await geminiSynthesize({ apiKey: p.geminiKey, api: p.geminiApi, model: ttsModel, voice: b.language.ttsVoice ?? "Kore", text, fetch: p.fetch });
      audio = sp.audio;
      await usage(db, {
        ...base, provider: "gemini", operation: "tts", model: ttsModel, status: "ok",
        items: [
          { unitType: "input_tokens", units: sp.usage.inputTokens },
          { unitType: "audio_output_tokens", units: sp.usage.outputTokens || ttsAudioTokens(durationMs(sp.audio)) },
        ],
        latencyMs: performance.now() - t0, requestId: sp.requestId,
        meta: { audioMs: durationMs(sp.audio), sampleRate: sp.audio.sampleRate },
      });
    } catch (e) {
      await usage(db, { ...base, provider: "gemini", operation: "tts", model: ttsModel, status: "error", items: [], latencyMs: performance.now() - t0, error: (e as Error).message });
      throw e;
    }
    stage = "store";
    await deliver(audio, "ai");
    return { msgId: msg.msg_id, outcome: "ready" };
  } catch (e) {
    const err = e instanceof ProviderError ? e : new ProviderError(stage, (e as Error).message, true);
    await log(db, "error", stage, err.message, null, { jobId: b.job.id, attempt: b.job.attempts, retryable: err.retryable }, lang);
    if (stage === "store") {
      // Our own storage hiccup: the provider work is done, so retrying the upload is cheap.
      if (err.retryable && b.job.attempts < MAX_TTS_ATTEMPTS) {
        await call(db, "worker_tts_retry", { p_job_id: b.job.id, p_msg_id: msg.msg_id, p_delay_sec: 2, p_error: err.message });
        return { msgId: msg.msg_id, outcome: "retry", error: err.message };
      }
    } else {
      // Live speech cannot wait for retries: the original audio now beats a translation 10 s
      // late. The failing model is cooled down so the next sentences skip it immediately too:
      // rate limit/quota -> the provider's retry time (60 s if none); overload/timeout/5xx ->
      // 15 s; rejected key -> 5 min. Other errors (a bad response) affect only this sentence.
      const seconds = err.status === 429 ? err.retryAfterSec ?? 60
        : err.status === 401 || err.status === 403 ? 300
        : err.retryable ? Math.max(15, err.retryAfterSec ?? 0)
        : 0;
      if (seconds > 0) {
        await call(db, "worker_provider_cooldown", { p_key: `gemini:${model}`, p_seconds: seconds, p_reason: err.message.slice(0, 300) });
      }
      if (err.status === 429) return await fallback(`${model} rate-limited`);
    }
    return await fallback(`${stage} failed: ${err.message.slice(0, 200)}`);
  }
}

export async function processTtsBatch(db: AiDb, p: Providers, opts: { maxJobs: number; vtSec: number; deadlineMs: number }) {
  const started = Date.now();
  const results: TtsResult[] = [];
  while (Date.now() - started < opts.deadlineMs) {
    const msgs = await call<TtsMessage[] | null>(db, "worker_claim_tts", { p_qty: opts.maxJobs, p_vt: opts.vtSec });
    if (!msgs || msgs.length === 0) break;
    results.push(...(await Promise.all(msgs.map((m) =>
      processTtsMessage(db, m, p).catch(async (e) => {
        await log(db, "error", "tts", (e as Error).message, null, { msgId: m.msg_id, jobId: m.job_id });
        return { msgId: m.msg_id, outcome: "error", error: (e as Error).message };
      })
    ))));
  }
  return { processed: results.length, results };
}
