// Worker tests with mocked providers and a scripted database. No network, no cost.
// Run: deno test supabase/functions/_shared/
import { assert, assertEquals, assertStringIncludes } from "jsr:@std/assert@1.0.13";
import { type AiDb, processSttBatch, processSttMessage, processTtsBatch, processTtsMessage, segmentPath } from "./ai_pipeline.ts";
import { decodeWav, encodeAdpcmWav, encodePcmWav, type PcmAudio } from "./audio.ts";
import type { FetchLike } from "./providers.ts";

// ------------------------------------------------------------------ fakes

interface Call {
  fn: string;
  args: Record<string, unknown>;
}

function tone(ms: number, rate = 16000, f = 220): PcmAudio {
  const n = Math.round((ms * rate) / 1000);
  const s = new Int16Array(n);
  for (let i = 0; i < n; i++) s[i] = Math.round(5000 * Math.sin((2 * Math.PI * f * i) / rate));
  return { sampleRate: rate, samples: s };
}

function fakeDb(handlers: Record<string, (args: Record<string, unknown>) => unknown>, files: Record<string, Uint8Array> = {}) {
  const calls: Call[] = [];
  const uploads: { bucket: string; path: string; body: Uint8Array; contentType?: string }[] = [];
  const db: AiDb = {
    rpc(fn, args = {}) {
      calls.push({ fn, args });
      const h = handlers[fn];
      return Promise.resolve({ data: h ? h(args) : null, error: null });
    },
    storage: {
      from(bucket: string) {
        return {
          download(path: string) {
            const f = files[`${bucket}/${path}`];
            return Promise.resolve(f ? { data: new Blob([f as Uint8Array<ArrayBuffer>]), error: null } : { data: null, error: { message: "Object not found", statusCode: "404" } });
          },
          upload(path: string, body: Uint8Array, o?: { contentType?: string }) {
            uploads.push({ bucket, path, body, contentType: o?.contentType });
            return Promise.resolve({ data: { path }, error: null });
          },
        };
      },
    },
  };
  const named = (fn: string) => calls.filter((c) => c.fn === fn);
  return { db, calls, uploads, named };
}

interface Req {
  url: string;
  init?: RequestInit;
  body?: unknown;
}

/** Routes provider requests to handlers; records every request (= every billable call). */
function fakeFetch(route: (url: string, body: unknown, n: number) => Response | Promise<Response>) {
  const reqs: Req[] = [];
  const f: FetchLike = async (url, init) => {
    let body: unknown = init?.body;
    if (typeof body === "string") body = JSON.parse(body);
    reqs.push({ url, init, body });
    return await route(url, body, reqs.length);
  };
  return { f, reqs, to: (host: string) => reqs.filter((r) => r.url.includes(host)) };
}

const json = (v: unknown, status = 200) => new Response(JSON.stringify(v), { status, headers: { "Content-Type": "application/json" } });

function deepgramResponse(words: [string, number, number][], language = "en") {
  return json({
    metadata: { request_id: "dg-1", duration: 5 },
    results: {
      channels: [{
        detected_language: language,
        alternatives: [{
          transcript: words.map((w) => w[0]).join(" "),
          confidence: 0.97,
          words: words.map(([w, s, e]) => ({ word: w.toLowerCase(), punctuated_word: w, start: s, end: e, confidence: 0.9, language })),
        }],
      }],
    },
  });
}

function geminiText(text: string) {
  return json({ id: "int-1", status: "completed", steps: [{ type: "thought", summary: [] }, { type: "model_output", content: [{ type: "text", text }] }], usage: { total_input_tokens: 120, total_output_tokens: 30, total_thought_tokens: 40 } });
}

function geminiAudio(ms: number) {
  const pcm = tone(ms).samples;
  const bytes = new Uint8Array(pcm.buffer.slice(0));
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return json({ id: "int-2", steps: [{ type: "model_output", content: [{ type: "audio", mime_type: "audio/l16;rate=16000", data: btoa(bin) }] }], usage: { total_input_tokens: 20, total_output_tokens: Math.ceil(ms / 40) } });
}

const PROV = { deepgramKey: "dg-test", geminiKey: "gm-test" };
const SESSION = "sess-1";
const HUB = "hub-1";
const chunk = (seq: number, attempts = 1) => ({ id: 100 + seq, sessionId: SESSION, hubId: HUB, seq, path: `${HUB}/${SESSION}/${String(seq).padStart(6, "0")}.wav`, durationMs: 5000, attempts });
const msg = (seq: number) => ({ msg_id: 900 + seq, read_ct: 1, chunk_id: 100 + seq, flush_session: null });
const sourceFiles = (...seqs: number[]) => Object.fromEntries(seqs.map((s) => [`source-audio/${chunk(s).path}`, encodeAdpcmWav(tone(5000))]));

// ------------------------------------------------------------------ STT stage

Deno.test("STT success: transcribes, assembles a sentence and commits it with usage", async () => {
  const { db, named } = fakeDb({
    worker_stt_begin: () => ({ action: "process", chunk: chunk(1), prev: null, isLast: false, state: {}, languages: ["af"], sttModel: "nova-3", sttLanguage: "multi" }),
    worker_stt_commit: () => ({ utterances: 1, jobs: 1 }),
  }, sourceFiles(1));
  const net = fakeFetch(() => deepgramResponse([["Energy", 0.2, 0.6], ["is", 0.7, 0.8], ["conserved.", 0.9, 1.5]]));
  const r = await processSttMessage(db, msg(1), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "done");
  assertEquals(net.to("deepgram").length, 1);
  const req = net.reqs[0];
  assertStringIncludes(req.url, "model=nova-3");
  assertStringIncludes(req.url, "language=multi");
  assertEquals((req.init?.headers as Record<string, string>).Authorization, "Token dg-test");
  const sent = decodeWav(req.init!.body as Uint8Array); // ADPCM decoded to PCM for Deepgram
  assertEquals(sent.samples.length, 80000);
  const commit = named("worker_stt_commit")[0].args;
  assertEquals(commit.p_status, "done");
  assertEquals((commit.p_utterances as { text: string }[]).map((u) => u.text), ["Energy is conserved."]);
  const usage = named("worker_usage")[0].args;
  assertEquals(usage.p_model, "nova-3/multi");
  assertEquals((usage.p_items as { units: number }[])[0].units.toFixed(4), (5 / 60).toFixed(4));
});

Deno.test("STT sends 1 s of the previous chunk as pre-roll when contiguous", async () => {
  const { db, named } = fakeDb({
    worker_stt_begin: () => ({ action: "process", chunk: chunk(2), prev: { seq: 1, path: chunk(1).path, durationMs: 5000 }, isLast: false, state: { prevSeq: 1, prevDurationSec: 5, prevCutSec: 4.4, held: [{ text: "conser-", start: 4.6, end: 4.95 }], carry: [] }, languages: ["af"] }),
    worker_stt_commit: () => ({ utterances: 1, jobs: 1 }),
  }, sourceFiles(1, 2));
  // Deepgram hears the whole word in the pre-roll (time 0.6-1.4 of the 6 s audio).
  const net = fakeFetch(() => deepgramResponse([["conserved.", 0.6, 1.4], ["Next", 1.6, 1.9]]));
  await processSttMessage(db, msg(2), { ...PROV, fetch: net.f });
  assertEquals(decodeWav(net.reqs[0].init!.body as Uint8Array).samples.length, 96000);
  const commit = named("worker_stt_commit")[0].args;
  // The pre-roll version of the word replaces the cut "conser-"; "Next" is then followed by 4 s of
  // silence, so the pause ends the utterance.
  assertEquals((commit.p_utterances as { text: string }[]).map((u) => u.text), ["conserved. Next"]);
  assertEquals((commit.p_transcript as { prerollMs: number }).prerollMs, 1000);
});

Deno.test("no listeners / silent / stale: no provider call at all", async () => {
  for (const reason of ["skipped_no_listeners", "skipped_silent", "skipped_stale"]) {
    const { db, named } = fakeDb({
      worker_stt_begin: () => ({ action: "skip", reason, chunk: chunk(3), state: {}, languages: [] }),
      worker_stt_commit: () => ({ utterances: 0, jobs: 0 }),
    });
    const net = fakeFetch(() => json({}));
    const r = await processSttMessage(db, msg(3), { ...PROV, fetch: net.f });
    assertEquals(r.outcome, reason);
    assertEquals(net.reqs.length, 0, reason);
    assertEquals(named("worker_stt_commit")[0].args.p_status, reason);
  }
});

Deno.test("duplicate delivery of a finished chunk is archived without any work", async () => {
  const { db, named } = fakeDb({ worker_stt_begin: () => ({ action: "final", reason: "done" }) });
  const net = fakeFetch(() => json({}));
  const r = await processSttMessage(db, msg(4), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "already_final");
  assertEquals(net.reqs.length, 0);
  assertEquals(named("worker_queue_archive").length, 1);
  assertEquals(named("worker_stt_commit").length, 0);
});

Deno.test("a chunk waits while its predecessor is unfinished (ordering)", async () => {
  const { db, named } = fakeDb({ worker_stt_begin: () => ({ action: "wait", reason: "predecessor" }) });
  const net = fakeFetch(() => json({}));
  const r = await processSttMessage(db, msg(5), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "wait_predecessor");
  assertEquals(net.reqs.length, 0);
  assertEquals(named("worker_queue_delay")[0].args, { p_queue: "chunk_jobs", p_msg_id: 905, p_delay_sec: 2 });
});

Deno.test("a late chunk is committed without touching the assembler state", async () => {
  const { db, named } = fakeDb({
    worker_stt_begin: () => ({ action: "skip", reason: "skipped_stale", late: true, chunk: chunk(2), state: { prevSeq: 9, held: [{ text: "x", start: 1, end: 2 }] } }),
    worker_stt_commit: () => ({ utterances: 0, jobs: 0 }),
  });
  await processSttMessage(db, msg(2), PROV);
  assertEquals(named("worker_stt_commit")[0].args.p_state, null);
});

Deno.test("STT transient failure: retried with backoff, nothing committed", async () => {
  const { db, named } = fakeDb({ worker_stt_begin: () => ({ action: "process", chunk: chunk(6, 1), state: {}, languages: ["af"] }) }, sourceFiles(6));
  const net = fakeFetch(() => new Response("upstream", { status: 503 }));
  const r = await processSttMessage(db, msg(6), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "retry");
  assertEquals(named("worker_stt_retry")[0].args.p_delay_sec, 2);
  assertEquals(named("worker_stt_commit").length, 0);
  assertEquals(named("worker_usage")[0].args.p_status, "error");
});

Deno.test("STT failure after the last attempt is dead-lettered; the session continues", async () => {
  const { db, named } = fakeDb({
    worker_stt_begin: () => ({ action: "process", chunk: chunk(7, 3), state: { prevSeq: 6, prevDurationSec: 5, prevCutSec: 4, held: [{ text: "pending.", start: 4.7, end: 4.9 }], carry: [] }, languages: ["af"] }),
    worker_stt_commit: () => ({ utterances: 1, jobs: 1 }),
  }, sourceFiles(7));
  const net = fakeFetch(() => new Response("bad gateway", { status: 502 }));
  const r = await processSttMessage(db, msg(7), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "failed");
  const c = named("worker_stt_commit")[0].args;
  assertEquals(c.p_status, "failed");
  assertEquals((c.p_utterances as { text: string }[]).map((u) => u.text), ["pending."]); // held words not lost
  assertEquals((c.p_state as { prevSeq: number }).prevSeq, 7);
});

Deno.test("STT bad key (401) is not retried", async () => {
  const { db, named } = fakeDb({ worker_stt_begin: () => ({ action: "process", chunk: chunk(8, 1), state: {}, languages: ["af"] }), worker_stt_commit: () => ({ utterances: 0 }) }, sourceFiles(8));
  const net = fakeFetch(() => new Response("invalid credentials", { status: 401 }));
  const r = await processSttMessage(db, msg(8), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "failed");
  assertEquals(named("worker_stt_retry").length, 0);
});

Deno.test("session end flush emits the unfinished sentence once", async () => {
  const { db, named } = fakeDb({
    worker_stt_flush_begin: () => ({ action: "process", nextSeq: 12, state: { prevSeq: 11, prevDurationSec: 2.5, prevCutSec: 1, held: [{ text: "goodbye", start: 1.2, end: 1.6 }], carry: [{ text: "and", seq: 11, start: 0.5, end: 0.8 }] } }),
    worker_stt_flush_commit: () => ({ utterances: 1 }),
  });
  const r = await processSttMessage(db, { msg_id: 1, read_ct: 1, chunk_id: null, flush_session: SESSION }, PROV);
  assertEquals(r.outcome, "flushed");
  assertEquals((named("worker_stt_flush_commit")[0].args.p_utterances as { text: string }[]).map((u) => u.text), ["and goodbye"]);
});

Deno.test("STT batch stops when every claimed message is waiting", async () => {
  let claims = 0;
  const { db } = fakeDb({
    worker_claim_stt: () => (claims++ < 5 ? [msg(1), msg(2)] : []),
    worker_stt_begin: () => ({ action: "wait", reason: "lease" }),
  });
  const r = await processSttBatch(db, PROV, { maxJobs: 2, vtSec: 60, deadlineMs: 5000 });
  assertEquals(claims, 1);
  assertEquals(r.processed, 2);
});

// ------------------------------------------------------------------ translation + TTS stage

function ttsBegin(over: Record<string, unknown> = {}) {
  return {
    action: "process",
    reason: null,
    method: "tts",
    job: { id: 55, attempts: 1, translatedText: null, language: "af" },
    utterance: { id: 7, seq: 3, text: "Energy is conserved.", language: "en", startSeq: 1, startMs: 200, endSeq: 1, endMs: 1500 },
    session: { id: SESSION, hubId: HUB },
    language: { code: "af", name: "Afrikaans", ttsModel: "gemini-3.8-flash-lite-tts", ttsVoice: "Kore" },
    sourceLanguageName: "English",
    context: [{ source: "Today we discuss thermodynamics.", target: "Vandag bespreek ons termodinamika." }],
    chunks: [],
    translateModel: "gemini-3.8-flash",
    translateThinking: "low",
    ...over,
  };
}
const ttsMsg = (job = 55) => ({ msg_id: 700 + job, read_ct: 1, job_id: job });

Deno.test("TTS success: one translation, one synthesis, ADPCM segment stored and committed", async () => {
  const { db, named, uploads } = fakeDb({ worker_tts_begin: () => ttsBegin() });
  const net = fakeFetch((_url, body) => {
    const b = body as Record<string, unknown>;
    return b.response_format ? geminiAudio(1800) : geminiText("Energie word behou.");
  });
  const r = await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "ready");
  assertEquals(net.reqs.length, 2);
  const tr = net.reqs[0].body as Record<string, unknown>;
  assertEquals(tr.model, "gemini-3.8-flash");
  assertEquals(tr.store, false); // lecture text is not stored by the provider
  assertStringIncludes(String(tr.system_instruction), "into Afrikaans (af)");
  assertStringIncludes(String(tr.system_instruction), "Do not summarise");
  assertStringIncludes(String(tr.input), "NEW SEGMENT:\nEnergy is conserved.");
  assertStringIncludes(String(tr.input), "Vandag bespreek ons termodinamika."); // context
  const tts = net.reqs[1].body as Record<string, unknown>;
  assertEquals(tts.model, "gemini-3.8-flash-lite-tts");
  assertEquals((tts.response_format as Record<string, unknown>).sample_rate, 16000);
  assertEquals(named("worker_tts_save_translation")[0].args.p_text, "Energie word behou.");
  assertEquals(uploads.length, 1);
  assertEquals(uploads[0].path, segmentPath(HUB, SESSION, "af", 3));
  const stored = decodeWav(uploads[0].body);
  assertEquals(stored.sampleRate, 16000);
  assertEquals(stored.samples.length, 28800);
  const commit = named("worker_tts_commit")[0].args;
  assertEquals([commit.p_status, commit.p_duration_ms], ["ready", 1800]);
  const usage = named("worker_usage").map((c) => [c.args.p_operation, c.args.p_items]);
  assertEquals(usage[0], ["translate", [{ unitType: "input_tokens", units: 120 }, { unitType: "output_tokens", units: 70 }]]);
  assertEquals(usage[1], ["tts", [{ unitType: "input_tokens", units: 20 }, { unitType: "audio_output_tokens", units: 45 }]]);
});

Deno.test("final listener left: queued job is skipped without any provider call", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => ttsBegin({ action: "skip", reason: "skipped_no_listeners" }) });
  const net = fakeFetch(() => json({}));
  const r = await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "skipped_no_listeners");
  assertEquals(net.reqs.length, 0);
  assertEquals(named("worker_tts_commit")[0].args.p_status, "skipped_no_listeners");
});

Deno.test("TTS error after a successful translation: original audio now, translation billed once and kept", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch((_u, body) =>
    (body as Record<string, unknown>).response_format ? new Response("timeout", { status: 504 }) : geminiText("Energie word behou."));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(net.reqs.length, 2); // one translation, one TTS attempt, no retries
  assertEquals(named("worker_tts_save_translation")[0].args.p_text, "Energie word behou.");
  assertEquals(named("worker_tts_retry").length, 0);
  assertEquals(named("worker_provider_cooldown")[0].args.p_seconds, 15);
});

Deno.test("a stored translation is reused: no second translation request", async () => {
  const { db } = fakeDb({ worker_tts_begin: () => withSource({ job: { id: 55, attempts: 2, translatedText: "Energie word behou.", language: "af" } }) }, sourceFiles(1));
  const net = fakeFetch(() => geminiAudio(1000));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "ready");
  assertEquals(net.reqs.length, 1);
  assert((net.reqs[0].body as Record<string, unknown>).response_format);
});

Deno.test("TTS gives up after the last attempt and records the failure", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => ttsBegin({ job: { id: 55, attempts: 3, translatedText: "Energie word behou.", language: "af" } }) });
  const net = fakeFetch(() => new Response("overloaded", { status: 503 }));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "failed");
  assertEquals(named("worker_tts_commit")[0].args.p_status, "failed");
  assertEquals(named("worker_tts_retry").length, 0);
});

Deno.test("a failing language does not stop another language", async () => {
  let claimed = false;
  const { db, named } = fakeDb({
    worker_claim_tts: () => (claimed ? [] : (claimed = true, [ttsMsg(1), ttsMsg(2)])),
    worker_tts_begin: (a) =>
      a.p_job_id === 1
        ? ttsBegin({ job: { id: 1, attempts: 3, translatedText: null, language: "st" }, language: { code: "st", name: "Sesotho", ttsModel: "gemini-3.8-flash-tts", ttsVoice: "Kore" } })
        : ttsBegin({ job: { id: 2, attempts: 1, translatedText: null, language: "af" } }),
  });
  const net = fakeFetch((_u, body) => {
    const b = body as Record<string, unknown>;
    if (String(b.system_instruction ?? "").includes("Sesotho")) return new Response("bad request", { status: 400 });
    return b.response_format ? geminiAudio(900) : geminiText("Energie word behou.");
  });
  const r = await processTtsBatch(db, { ...PROV, fetch: net.f }, { maxJobs: 2, vtSec: 90, deadlineMs: 5000 });
  const byJob = Object.fromEntries(named("worker_tts_commit").map((c) => [c.args.p_job_id, c.args.p_status]));
  assertEquals(byJob, { 1: "failed", 2: "ready" });
  assertEquals(r.processed, 2);
});

Deno.test("same language as the lecturer: the lecturer's own audio, no provider call", async () => {
  const files = sourceFiles(1, 2);
  const { db, uploads, named } = fakeDb({
    worker_tts_begin: () => ttsBegin({
      method: "passthrough",
      job: { id: 9, attempts: 1, translatedText: null, language: "en" },
      language: { code: "en", name: "English", ttsModel: "gemini-3.8-flash-lite-tts", ttsVoice: "Kore" },
      utterance: { id: 7, seq: 4, text: "x", language: "en", startSeq: 1, startMs: 4000, endSeq: 2, endMs: 1000 },
      chunks: [{ seq: 1, path: chunk(1).path, durationMs: 5000 }, { seq: 2, path: chunk(2).path, durationMs: 5000 }],
    }),
  }, files);
  const net = fakeFetch(() => json({}));
  assertEquals((await processTtsMessage(db, ttsMsg(9), { ...PROV, fetch: net.f })).outcome, "ready");
  assertEquals(net.reqs.length, 0);
  // 4.0 s..5.0 s of chunk 1 + 0..1.0 s of chunk 2, plus 150 ms padding each side
  assertEquals(named("worker_tts_commit")[0].args.p_duration_ms, 2300);
  assertEquals(uploads[0].path, segmentPath(HUB, SESSION, "en", 4));
});

Deno.test("generated audio paths stay inside the hub/session folder", () => {
  assertEquals(segmentPath("h", "s", "af", 12), "h/s/af/u000012.wav");
});

Deno.test("PCM and ADPCM uploads are both accepted as source audio", async () => {
  for (const wav of [encodePcmWav(tone(5000)), encodeAdpcmWav(tone(5000))]) {
    const { db } = fakeDb({
      worker_stt_begin: () => ({ action: "process", chunk: chunk(1), state: {}, languages: ["af"] }),
      worker_stt_commit: () => ({ utterances: 0 }),
    }, { [`source-audio/${chunk(1).path}`]: wav });
    const net = fakeFetch(() => deepgramResponse([]));
    assertEquals((await processSttMessage(db, msg(1), { ...PROV, fetch: net.f })).outcome, "done");
    assert(decodeWav(net.reqs[0].init!.body as Uint8Array).samples.length === 80000);
  }
});

// ------------------------------------------------------------------ original-audio fallback

// Utterance spans 0.2 s..1.5 s of chunk 1 -> 1.3 s + 2 x 150 ms padding of the lecturer's audio.
const withSource = (over: Record<string, unknown> = {}) =>
  ttsBegin({ chunks: [{ seq: 1, path: chunk(1).path, durationMs: 5000 }], chunksComplete: true, ...over });

Deno.test("fallback: Gemini rate limit -> model cooled down, listeners get the original audio at once", async () => {
  const { db, named, uploads } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch(() => new Response(JSON.stringify({ error: { message: "Rate limit exceeded (limit: 5 requests per minute on Free Tier). Please retry in 32s" } }), { status: 429 }));
  const r = await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f });
  assertEquals(r.outcome, "fallback");
  assertEquals(net.reqs.length, 1); // no retries that would burn quota
  assertEquals(named("worker_provider_cooldown")[0].args, { p_key: "gemini:gemini-3.8-flash", p_seconds: 32, p_reason: named("worker_provider_cooldown")[0].args.p_reason });
  assertEquals(named("worker_tts_retry").length, 0);
  const c = named("worker_tts_commit")[0].args;
  assertEquals([c.p_status, c.p_mode, c.p_duration_ms], ["ready", "fallback", 1600]);
  assertEquals(uploads[0].path, segmentPath(HUB, SESSION, "af", 3));
});

Deno.test("fallback: while a model is cooling down, Gemini is not called at all", async () => {
  const until = new Date(Date.now() + 30_000).toISOString();
  for (const cooldowns of [{ translate: until, tts: null }, { translate: null, tts: until }]) {
    const { db, named } = fakeDb({ worker_tts_begin: () => withSource({ cooldowns }) }, sourceFiles(1));
    const net = fakeFetch(() => geminiText("unused"));
    const r = await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f });
    assertEquals([r.outcome, net.reqs.length], ["fallback", 0]);
    assertEquals(named("worker_tts_commit")[0].args.p_mode, "fallback");
  }
});

Deno.test("fallback: TTS rate-limited after a successful translation still gives original audio", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch((_u, body) =>
    (body as Record<string, unknown>).response_format ? new Response("quota", { status: 429 }) : geminiText("Energie word behou."));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(named("worker_provider_cooldown")[0].args.p_key, "gemini:gemini-3.8-flash-lite-tts");
  assertEquals(named("worker_provider_cooldown")[0].args.p_seconds, 60); // no hint -> 60 s
});

Deno.test("fallback: overload (503) -> original audio at once and a 15 s cool-down", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch(() => new Response("high demand", { status: 503 }));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(net.reqs.length, 1);
  assertEquals(named("worker_tts_retry").length, 0);
  assertEquals(named("worker_provider_cooldown")[0].args.p_seconds, 15);
  assertEquals(named("worker_tts_commit")[0].args.p_mode, "fallback");
});

Deno.test("fallback: a rejected key cools the model down for 5 minutes", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch(() => new Response("API key not valid", { status: 403 }));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(named("worker_provider_cooldown")[0].args.p_seconds, 300);
});

Deno.test("fallback: a bad response (400) goes straight to original audio, no cool-down", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource() }, sourceFiles(1));
  const net = fakeFetch(() => new Response("bad request", { status: 400 }));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(named("worker_tts_retry").length, 0);
  assertEquals(named("worker_provider_cooldown").length, 0);
});

Deno.test("fallback: over the cost cap, original audio without any provider call", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => withSource({ method: "fallback" }) }, sourceFiles(1));
  const net = fakeFetch(() => geminiText("unused"));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "fallback");
  assertEquals(net.reqs.length, 0);
  assertEquals(named("worker_tts_commit")[0].args.p_mode, "fallback");
});

Deno.test("fallback impossible (source audio already deleted): job fails, nothing played", async () => {
  const { db, named } = fakeDb({ worker_tts_begin: () => ttsBegin({ chunks: [] }) });
  const net = fakeFetch(() => new Response("quota", { status: 429 }));
  assertEquals((await processTtsMessage(db, ttsMsg(), { ...PROV, fetch: net.f })).outcome, "failed");
  assertEquals(named("worker_tts_commit")[0].args.p_status, "failed");
});
