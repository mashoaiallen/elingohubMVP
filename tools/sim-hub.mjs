#!/usr/bin/env node
// Simulated Hub: drives the real Supabase project with exactly the requests the ESP32 makes,
// to verify the cloud data path without hardware (Milestone 1, loopback pipeline).
//
//   node tools/sim-hub.mjs --creds tools/.hub-credentials-<slug>.json [--pcm]
//
// Chunks are uploaded as IMA ADPCM like firmware >= 0.3.0 (--pcm: 16-bit PCM like 0.2.x).
//
// It uses synthetic test tones, never lecture audio. Prints PASS/FAIL per check and timings.
import { readFileSync } from "node:fs";
import { HubClient, http, loadEnv, makeWav, parseArgs, sleep } from "./lib.mjs";
// Node >= 22.18 strips TypeScript types: reuse the cloud's encoder (byte-identical to the Hub's).
import { decodeWav, encodeAdpcmWav } from "../supabase/functions/_shared/audio.ts";

const args = parseArgs(process.argv.slice(2));
if (typeof args.creds !== "string") {
  console.error("Usage: node tools/sim-hub.mjs --creds tools/.hub-credentials-<slug>.json");
  process.exit(2);
}
const creds = JSON.parse(readFileSync(args.creds, "utf8"));
const env = loadEnv();
const hub = new HubClient(creds.supabaseUrl, creds.publishableKey, creds.hubEmail, creds.hubPassword);

let failures = 0;
const check = (ok, label, detail = "") => {
  if (!ok) failures++;
  console.log(`${ok ? "PASS" : "FAIL"}  ${label}${detail ? `  (${detail})` : ""}`);
};
const pad = (n) => String(n).padStart(6, "0");
const timings = [];

async function sendChunk(sessionId, hubId, seq, freq) {
  const wav = args.pcm ? makeWav(5000, freq) : encodeAdpcmWav(decodeWav(makeWav(5000, freq)));
  const t0 = performance.now();
  await hub.upload("source-audio", `${hubId}/${sessionId}/${pad(seq)}.wav`, wav);
  const tUp = performance.now();
  const reg = await hub.rpc("hub_register_chunk", {
    p_session_id: sessionId, p_seq: seq, p_duration_ms: 5000, p_rms: 4200, p_peak: 6000, p_silent: false,
    p_captured_at: new Date().toISOString(),
  });
  return { wav, reg, uploadMs: tUp - t0, registerMs: performance.now() - tUp, registeredAt: performance.now() };
}

async function waitForSegments(sessionId, after, langs, wantCount, timeoutMs = 30000) {
  const started = performance.now();
  let found = [];
  while (performance.now() - started < timeoutMs) {
    const r = await hub.rpc("hub_poll", { p_session_id: sessionId, p_after_segment_id: after, p_languages: langs });
    found = r.segments;
    if (found.length >= wantCount) return { segments: found, waitedMs: performance.now() - started, poll: r };
    await sleep(500);
  }
  return { segments: found, waitedMs: performance.now() - started };
}

async function chunkStatus(chunkId) {
  // Read with the secret key (diagnostic only; the Hub itself cannot read tables).
  const r = await http("GET", `${env.SUPABASE_URL}/rest/v1/source_chunks?id=eq.${chunkId}&select=status,target_languages`, {
    headers: { apikey: env.SUPABASE_SECRET_KEY },
  });
  return r.data?.[0];
}

// ------------------------------------------------------------------------------------- run
const tSignIn = performance.now();
await hub.signIn();
console.log(`Signed in as hub "${creds.name}" in ${(performance.now() - tSignIn).toFixed(0)} ms`);

const hello = await hub.rpc("hub_hello", { p_firmware_version: "sim-hub", p_stats: { sim: true } });
check(hello.hub.id === creds.hubId, "hub_hello identifies this hub");
check(hello.languages.length >= 1, "language catalog returned", hello.languages.map((l) => l.code).join(","));

const nonce = `sim${Date.now().toString(36)}`;
const session = await hub.rpc("hub_start_session", { p_client_nonce: nonce });
const again = await hub.rpc("hub_start_session", { p_client_nonce: nonce });
check(session.id === again.id, "start_session retried with the same nonce returns the same session");
const sid = session.id;

// Two phones on Afrikaans, one on Sesotho.
let sync = await hub.rpc("hub_sync", {
  p_session_id: sid,
  p_listeners: [{ id: "simphone-0001", lang: "af" }, { id: "simphone-0002", lang: "af" }, { id: "simphone-0003", lang: "st" }],
});
check(JSON.stringify(sync.languages) === JSON.stringify([{ code: "af", listeners: 2 }, { code: "st", listeners: 1 }]),
  "two phones on af + one on st => af=2, st=1", JSON.stringify(sync.languages));

// Chunk 1: both languages active => exactly one segment per language.
const c1 = await sendChunk(sid, creds.hubId, 1, 440);
check(!c1.reg.duplicate, "chunk 1 registered", `upload ${c1.uploadMs.toFixed(0)} ms, register ${c1.registerMs.toFixed(0)} ms`);
const s1 = await waitForSegments(sid, 0, ["af", "st"], 2);
check(s1.segments.length === 2, "chunk 1 produced one shared segment per active language",
  s1.segments.map((s) => s.lang).join(","));
timings.push({ chunk: 1, uploadMs: c1.uploadMs, registerMs: c1.registerMs, registerToReadyMs: s1.waitedMs });

const tDl = performance.now();
const bytes = s1.segments.length ? await hub.download("translated-audio", s1.segments[0].path) : new Uint8Array();
const dlMs = performance.now() - tDl;
check(bytes.length === c1.wav.length && Buffer.compare(Buffer.from(bytes), Buffer.from(c1.wav)) === 0,
  "downloaded segment is byte-identical to the uploaded chunk (loopback)", `${bytes.length} bytes in ${dlMs.toFixed(0)} ms`);

// Duplicate registration must not create a second job/segment.
const dup = await hub.rpc("hub_register_chunk", { p_session_id: sid, p_seq: 1, p_duration_ms: 5000 });
check(dup.duplicate === true && dup.chunkId === c1.reg.chunkId, "duplicate chunk registration is idempotent");

// The final Sesotho listener leaves: chunk 2 must only produce Afrikaans.
sync = await hub.rpc("hub_sync", {
  p_session_id: sid,
  p_listeners: [{ id: "simphone-0001", lang: "af" }, { id: "simphone-0002", lang: "af" }],
});
check(JSON.stringify(sync.languages) === JSON.stringify([{ code: "af", listeners: 2 }]), "final st listener left => st inactive");
const lastId = Math.max(...s1.segments.map((s) => s.id));
const c2 = await sendChunk(sid, creds.hubId, 2, 523);
const s2 = await waitForSegments(sid, lastId, ["af", "st"], 1);
await sleep(3000); // give a wrongly-generated st segment time to appear
const s2b = await hub.rpc("hub_poll", { p_session_id: sid, p_after_segment_id: lastId, p_languages: ["af", "st"] });
check(s2b.segments.length === 1 && s2b.segments[0].lang === "af", "chunk 2: only af generated, no st work",
  s2b.segments.map((s) => s.lang).join(","));
timings.push({ chunk: 2, uploadMs: c2.uploadMs, registerMs: c2.registerMs, registerToReadyMs: s2.waitedMs });

// One af listener leaves while the other remains: af stays active.
sync = await hub.rpc("hub_sync", { p_session_id: sid, p_listeners: [{ id: "simphone-0002", lang: "af" }] });
check(JSON.stringify(sync.languages) === JSON.stringify([{ code: "af", listeners: 1 }]), "one of two af listeners leaves => af=1 still active");

// Everyone leaves: the next chunk must not generate anything.
await hub.rpc("hub_sync", { p_session_id: sid, p_listeners: [] });
const c3 = await sendChunk(sid, creds.hubId, 3, 659);
let st3;
for (let i = 0; i < 40; i++) {
  st3 = await chunkStatus(c3.reg.chunkId);
  if (st3 && !["queued", "processing"].includes(st3.status)) break;
  await sleep(500);
}
check(st3?.status === "skipped_no_listeners", "no listeners => chunk skipped, nothing generated", st3?.status);

// Security: the hub cannot read another hub's audio path or tables directly.
const foreign = await http("GET", `${creds.supabaseUrl}/storage/v1/object/authenticated/source-audio/00000000-0000-0000-0000-000000000000/x/000001.wav`, { headers: hub.headers() });
check(foreign.status >= 400, "hub cannot read another hub's storage folder", `HTTP ${foreign.status}`);
const table = await http("GET", `${creds.supabaseUrl}/rest/v1/sessions?select=id`, { headers: hub.headers() });
check(table.status >= 400 || (Array.isArray(table.data) && table.data.length === 0), "hub cannot read tables directly", `HTTP ${table.status}`);
const anon = await http("POST", `${creds.supabaseUrl}/rest/v1/rpc/hub_hello`, { headers: { apikey: creds.publishableKey }, body: {} });
check(anon.status >= 400, "anonymous caller cannot use the hub API", `HTTP ${anon.status}`);

const ended = await hub.rpc("hub_end_session", { p_session_id: sid });
check(ended.status === "ended", "session ended");
const after = await hub.rpc("hub_sync", { p_session_id: sid, p_listeners: [{ id: "simphone-0001", lang: "af" }] });
check(after.sessionStatus === "ended", "sync after end reports the session as ended");

console.log("\nTimings (cloud only, from this computer):");
for (const t of timings) {
  console.log(`  chunk ${t.chunk}: upload ${t.uploadMs.toFixed(0)} ms, register ${t.registerMs.toFixed(0)} ms, ` +
    `register->segment ready ${t.registerToReadyMs.toFixed(0)} ms (includes 500 ms poll granularity)`);
}
console.log(`\n${failures === 0 ? "ALL CHECKS PASSED" : `${failures} CHECK(S) FAILED`}`);
process.exit(failures === 0 ? 0 : 1);
