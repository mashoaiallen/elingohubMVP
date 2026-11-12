#!/usr/bin/env node
// Real end-to-end check of the AI pipeline WITHOUT hardware: plays the role of a Hub that streams
// a speech recording (16 kHz mono WAV) in real time as 5 s ADPCM chunks, with simulated phones on
// the given languages, then downloads the translated segments.
//
//   node tools/ai-smoke.mjs --creds tools/.hub-credentials-<hub>.json --wav speech.wav \
//        [--langs af,af,st] [--out <dir>]
//
// This calls the real providers (Deepgram + Gemini) through the deployed functions and costs
// money: roughly US$0.002 per 30 s clip per language. Use a recording you are allowed to send.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { HubClient, parseArgs, sleep } from "./lib.mjs";
import { decodeWav, encodeAdpcmWav, encodePcmWav } from "../supabase/functions/_shared/audio.ts";

const args = parseArgs(process.argv.slice(2));
if (typeof args.creds !== "string" || typeof args.wav !== "string") {
  console.error("Usage: node tools/ai-smoke.mjs --creds <hub creds json> --wav <16 kHz mono wav> [--langs af,af,st] [--out dir]");
  process.exit(2);
}
const creds = JSON.parse(readFileSync(args.creds, "utf8"));
const langs = String(args.langs ?? "af,af,st").split(",");
const outDir = typeof args.out === "string" ? args.out : path.join(path.dirname(args.wav), "ai-smoke-out");
mkdirSync(outDir, { recursive: true });

const audio = decodeWav(new Uint8Array(readFileSync(args.wav)));
if (audio.sampleRate !== 16000) throw new Error(`need 16 kHz audio, got ${audio.sampleRate}`);
const CHUNK = 80000;
const chunks = [];
for (let o = 0; o < audio.samples.length; o += CHUNK) chunks.push(audio.samples.slice(o, o + CHUNK));

const hub = new HubClient(creds.supabaseUrl, creds.publishableKey, creds.hubEmail, creds.hubPassword);
await hub.signIn();
const hello = await hub.rpc("hub_hello", { p_firmware_version: "ai-smoke", p_stats: {} });
const hubId = hello.hub.id;
const session = await hub.rpc("hub_start_session", { p_client_nonce: `smoke${Date.now().toString(36)}` });
const listeners = langs.map((lang, i) => ({ id: `smokeListener${String(i).padStart(3, "0")}`, lang }));
console.log(`session ${session.id}: ${chunks.length} chunks, listeners ${langs.join(",")}`);
await hub.rpc("hub_sync", { p_session_id: session.id, p_listeners: listeners });

const started = Date.now();
const uploadedAt = new Map();
const got = new Map(); // id -> segment
let after = 0;
const active = [...new Set(langs)];

async function poll() {
  const r = await hub.rpc("hub_poll", { p_session_id: session.id, p_after_segment_id: after, p_languages: active });
  for (const s of r.segments) {
    after = Math.max(after, s.id);
    if (got.has(s.id)) continue;
    const bytes = await hub.download("translated-audio", s.path);
    const pcm = decodeWav(bytes);
    const file = path.join(outDir, `${s.lang}-${String(s.seq).padStart(3, "0")}.wav`);
    writeFileSync(file, encodePcmWav(pcm)); // PCM copy so any player can open it
    const delay = (Date.now() - uploadedAt.get(s.chunkSeq)) / 1000;
    got.set(s.id, { ...s, bytes: bytes.length, rate: pcm.sampleRate, ms: Math.round(pcm.samples.length / pcm.sampleRate * 1000), delay });
    console.log(`  ${s.lang} #${s.seq} ${s.original ? "ORIGINAL AUDIO (translation unavailable)" : "translated"} (ends in chunk ${s.chunkSeq}): ${bytes.length} B, ${pcm.sampleRate} Hz, ` +
      `${(pcm.samples.length / pcm.sampleRate).toFixed(1)} s audio, ready ${delay.toFixed(1)} s after that chunk was uploaded`);
  }
}

for (let i = 0; i < chunks.length; i++) {
  const seq = i + 1;
  // Real time: chunk n is complete 5 s after chunk n-1, as on the Hub.
  const due = started + seq * 5000;
  while (Date.now() < due) {
    await poll();
    await sleep(500);
  }
  const wav = encodeAdpcmWav({ sampleRate: 16000, samples: chunks[i] });
  await hub.upload("source-audio", `${hubId}/${session.id}/${String(seq).padStart(6, "0")}.wav`, wav);
  await hub.rpc("hub_register_chunk", {
    p_session_id: session.id, p_seq: seq, p_duration_ms: Math.round(chunks[i].length / 16), p_rms: 1000, p_peak: 20000,
    p_silent: false, p_captured_at: new Date(started + i * 5000).toISOString(),
  });
  uploadedAt.set(seq, Date.now());
  console.log(`chunk ${seq} uploaded (${wav.length} B)`);
  if (i % 2 === 1) await hub.rpc("hub_sync", { p_session_id: session.id, p_listeners: listeners });
}
// Keep listening (and keep presence alive) for the tail, then end the session.
const tailUntil = Date.now() + 45000;
while (Date.now() < tailUntil) {
  await poll();
  await hub.rpc("hub_sync", { p_session_id: session.id, p_listeners: listeners });
  await sleep(1000);
}
await hub.rpc("hub_end_session", { p_session_id: session.id });
await sleep(8000);
await poll().catch(() => undefined);

const per = Object.fromEntries(active.map((l) => [l, [...got.values()].filter((s) => s.lang === l)]));
console.log("\nSummary");
for (const [l, segs] of Object.entries(per)) {
  const d = segs.map((s) => s.delay).sort((a, b) => a - b);
  console.log(`  ${l}: ${segs.length} segments (${segs.filter((s) => s.original).length} original audio), ${(segs.reduce((n, s) => n + s.ms, 0) / 1000).toFixed(1)} s audio` +
    (d.length ? `, delay after chunk upload min ${d[0].toFixed(1)} / median ${d[Math.floor(d.length / 2)].toFixed(1)} / max ${d[d.length - 1].toFixed(1)} s` : ""));
}
console.log(`session ${session.id}\naudio written to ${outDir}`);
process.exit(Object.values(per).every((s) => s.length > 0) ? 0 : 1);
