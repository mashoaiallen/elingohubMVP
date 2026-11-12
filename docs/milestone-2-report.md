# Milestone 2 – AI translation: implementation report

Branch `feature/m2-ai-translation` (from `feature/standalone-hub`). Status at the time of
writing: **implemented, deployed, automatically tested; physical validation pending**. Milestone
2 is not complete until the physical tests in [milestone-2-test-manual.md](milestone-2-test-manual.md)
pass on the real Hub with two phones.

## 1. Architecture

```
INMP441 ─▶ ESP32-S3 (16 kHz PCM → IMA ADPCM, 40 KB / 5 s) ──HTTPS──▶ Storage source-audio + hub_register_chunk
                                                                         │ pgmq chunk_jobs
                                                                         ▼
                                   process-chunk (STT stage, one session at a time, in seq order)
                                     Deepgram Nova-3 pre-recorded, chunk + 1 s pre-roll of the previous chunk
                                     → assembler: boundary de-duplication, whole sentences ("utterances")
                                                                         │ one translation_job per (utterance, ACTIVE language)
                                                                         ▼ pgmq tts_jobs
                                   process-translation (one job per invocation, languages in parallel)
                                     Gemini 3.8 Flash translation (with previous sentences as context)
                                     → Gemini TTS (16 kHz PCM) → IMA ADPCM WAV → Storage translated-audio
                                     or, if the student chose the lecturer's language: the lecturer's own audio
                                                                         │ segments (gated: in order per language)
                                                                         ▼
ESP32-S3 ◀── hub_poll + download (only languages its phones chose) ── phones at http://192.168.4.1
```

Unchanged from Milestone 1: AP+STA Hub, student page served by the Hub, Supabase as the only
backend, Hub authentication and RLS, provider keys only in Supabase secrets.

## 2. Phase reports

### Phase 1 – Milestone 1 baseline audit and closure

Evidence came from the cloud records of the physical Hub (17 sessions on 2026-11-12, read-only
queries); see the table in [milestone-1-test-plan.md](milestone-1-test-plan.md#field-data-from-2026-11-12).

| Finding | Status |
| --- | --- |
| Chunks with RMS 0 / peak 0: whole stretches of exact digital silence (e.g. 71 of 73 chunks), alternating with good audio, on both I2S slots | **Open (hardware).** An intermittent INMP441 connection. Firmware now reports `NO SIGNAL`, `deadChunks`. Silent chunks are never sent to Deepgram (no cost) |
| "send payload failed" and uploads 47–71 s behind with 2 languages, 14/130 chunks dropped | **Root cause found and fixed**: the core's lwIP has a 5.7 KB TCP window (sdkconfig `LWIP_TCP_SND_BUF_DEFAULT=5744`, `WND=5760`, no scaling) → ~30–60 KB/s per connection, while PCM needed 32 KB/s up + 32 KB/s down per language. Now: ADPCM both ways (8 KB/s per stream), downloads interleaved with uploads, stale audio skipped |
| Audio replayed (whole lecture after a Hub reboot; old segments for a late joiner; last segment after pause) | **Fixed** and browser-tested (no segment ever played twice) |
| Hub restarts mid-session (cause unknown) | Reset reason now reported in `hubs.last_stats.reset` |

Files: `firmware/esp32_hub/{audio_manager,cloud_sync,segment_store,local_server,config}.*`,
`data/app.js`, `data/index.html`, `tools/mock-hub.mjs`, docs.

### Phase 2 – Deepgram transcription

**Decision: pre-recorded API per 5 s chunk, with a 1 s pre-roll and timestamp stitching, not
streaming.** Reasons:

- Streaming needs one WebSocket for the whole lecture. The Hub must not hold the key, and an Edge
  Function cannot hold a connection for a lecture: the wall-clock limit is 150 s (free) or 400 s
  (paid). A permanent relay server is ruled out by the architecture.
- Chunk boundaries are handled explicitly. Each chunk is sent with the last 1 s of the previous
  chunk. Words ending in the last 0.35 s of a chunk are "held", and the next chunk takes over
  from the exact point where the previous one stopped committing. Tests check every boundary
  offset: each word is committed exactly once.
- Cost: pre-recorded multilingual is $0.0052/min, against $0.0092/min for streaming
  multilingual (Deepgram pricing, 2026-11-12). The pre-roll adds 20% (about $0.37 per lecture
  hour), still well below streaming.
- Latency: one HTTPS request per 5 s chunk (expected 0.5–1.5 s, to be measured).
- Model: `nova-3`, `language=multi` (code-switching). Both are in `app_settings` and can be changed
  without a deploy.

Sentence context: committed words collect until a sentence ends (`. ? !`), the lecturer pauses
for at least 1 s, or the sentence is 10 s old. Only whole utterances go to translation. Chunks are
processed strictly in order per session (SQL lease + cursor).

### Phase 3 – Gemini translation

- Model **`gemini-3.8-flash`** (stable), `thinking_level=low`. It is the strongest stable Flash
  model, and translation into Sesotho, Sepedi and siSwati needs quality more than the last few
  hundred ms. `gemini-3.5-flash-lite` (stable, cheaper, minimal thinking) is a one-row change in
  `app_settings` if measurements favour it.
- API: the Interactions API (`POST /v1beta/interactions`), recommended by Google for new work,
  with `store: false`. Without it, the API keeps interactions for 55 days on the paid tier.
  `generateContent` is still supported and can be selected with the secret `GEMINI_API=generate_content`.
- Prompt: complete and faithful, no summarising, keep technical terms, numbers and names.
  Previous segments are given as context only, with the rule never to repeat them, and the
  output is plain text.
- Context: the three previous utterances and their translations in the same language.
- Text is stored before TTS, so a TTS retry never pays for the translation again.

### Phase 4 – Gemini TTS

| Model (docs 2026-11-12) | af | st | nso | ss | en | Price (audio out) |
| --- | --- | --- | --- | --- | --- | --- |
| `gemini-3.8-flash-tts` | ✓ | ✓ | ✓ | ✓ | ✓ | $9 / 1M tokens = $0.00225 per 10 s |
| `gemini-3.8-flash-lite-tts` | ✓ | ✗ | ✓ | ✗ | ✓ | $6 / 1M tokens = $0.0015 per 10 s |

The per-language model is in `languages.tts_model`: Flash-Lite TTS for en, af and nso, and Flash
TTS for st and ss (Flash-Lite does not support them). The voice is `Kore` (in `languages.tts_voice`).

Output: `response_format {type: audio, mime_type: audio/l16, sample_rate: 16000}` gives 16 kHz
16-bit mono PCM. The worker encodes it as IMA ADPCM WAV (about 8 KB/s). **Player compatibility:**
browsers cannot decode ADPCM, so the student page decodes it itself (`data/adpcm.js`, tested
sample-for-sample against the cloud decoder and played in Chrome).

MP3 was evaluated and rejected: lamejs needed 2.2–3.4 s of CPU per 10 s of audio, against the
2 s CPU limit of Edge Functions.

When the student chose the lecturer's own language, the lecturer's real audio for that utterance
is cut from the source chunks: no translation and no TTS, at zero cost.

The real-API verification of these response shapes is the opt-in live test (section 4). It has
not been run yet because the provider keys are not configured.

### Phase 5 – Student delivery

The page is unchanged apart from ADPCM decoding, the earphones notice and the never-replay
rule. Segments arrive in speaking order: `hub_poll` holds a segment back while an earlier one of
the same language is still being produced (at most 15 s), and returns batches sorted by
utterance. Reconnection, duplicate-subscription and status handling are as in Milestone 1.

### Phase 6 – Demand-driven processing and cost controls

- **No listeners:** chunks are marked `skipped_no_listeners` before any Deepgram call, and no
  utterances or jobs are created.
- **One stream per language:** `_emit_utterances` creates one job per (utterance, active
  language), with a unique constraint. Listener counts come from `language_streams` (database).
- **Last listener leaves:** `worker_tts_begin` re-checks demand right before the provider call, so
  queued jobs become `skipped_no_listeners` with no charge.
- **Stale audio** (captured more than 45 s ago, or utterances older than 60 s) is never sent to a provider.
- **Silent chunks** are never transcribed.
- **Session cost cap** (`app_settings.session_cost_cap_usd`, default $25): above it, no provider calls.
- **Retries:** at most 3 provider attempts with backoff (2, 4, 8 s). 4xx errors other than
  408/429 are not retried.
- **Usage ledger** (`usage_events`, one row per call and unit) records audio minutes, tokens,
  audio tokens, latency, attempt, status, request id and estimated cost from `rate_cards`
  (official prices with validity dates, including the 2027-01-01 Gemini increases).
  `session_usage` and `session_summary` views report per session.

Estimated cost per lecture hour (assuming ~45 min of speech; **to be confirmed by measurement**):

| Item | Per lecture hour |
| --- | --- |
| Deepgram (once, shared by all languages; +20% pre-roll) | ~$0.37 |
| Translation per language (~600 utterances × ~500 in / ~150 out tokens) | ~$0.56 |
| TTS per language, Flash-Lite (af, nso, en) / Flash (st, ss) | ~$0.41 / ~$0.61 |
| Student on the lecturer's own language | $0 |
| **Example: Afrikaans + Sesotho** | **~$2.50** |

From 2027-01-01 the Gemini parts roughly double (already in `rate_cards`).

### Phase 7 – Reliability, security and cleanup

- **Idempotency:** unique `(session, seq)`, `(utterance, language)` and segment keys. A duplicate
  queue delivery is a no-op, and a stored translation is never overwritten.
- **Order:** an SQL lease plus a cursor per session for STT, and per-language gating for delivery.
- **Failures:** a failed chunk or job is marked `failed` with its error and archived (dead letter),
  and the session continues. Each language is a separate job, so one language failing never
  blocks another.
- **Upstream outages:** the Hub queues about 60 s of audio. Anything too far behind live is skipped,
  both in the cloud (no cost) and on the Hub (no download).
- **Security:**
  - Provider keys are Edge Function secrets only.
  - The new tables have RLS and no grants to anon or authenticated users, and the views are
    `security_invoker` with access revoked.
  - Worker functions are `service_role` only, and a segment path must sit inside its hub/session
    folder.
  - Edge Functions reject callers without the secret key (verified live: 401 without a key or
    with the publishable key).
  - Advisors: 0 errors, no new warnings.
- **Retention:** source audio is deleted 10 min after processing and translated audio 15 min
  after it is ready (unchanged). New: transcripts, utterances and translations are erased after
  24 h (`purge_lecture_text`, hourly cron). Provider-side storage is off (`store:false`).

### Phase 8 – Physical validation

**Not done.** See [milestone-2-test-manual.md](milestone-2-test-manual.md). Required: rewired
microphone, firmware 0.3.0, provider keys, two phones.

## 3. Changes

**Migration** `supabase/migrations/20261113090000_m2_ai_pipeline.sql` (applied 2026-11-12):
- New tables: `app_settings`, `utterances`, `translation_jobs`.
- New columns on `sessions` (STT cursor, lease, assembler state, cost), `transcripts` (words,
  pre-roll), `segments` (`utterance_id`, `chunk_seq`, `mime`) and `usage_events` (status,
  latency, attempt, error).
- New queue `tts_jobs`.
- Worker RPCs `worker_stt_*`, `worker_tts_*` and `worker_usage`.
- New `hub_poll` (ordered and gated, plus `chunkSeq`; backwards-compatible) and
  `_end_session` (queues the final-sentence flush).
- Rate cards, the TTS model per language, the hourly text purge, and the views `session_usage`
  and `session_summary`.

**Edge Functions:**
- `process-chunk`: the STT stage in AI mode, loopback otherwise.
- `process-translation` (new).
- `cleanup`: unchanged.
- Mode `auto`: AI when `DEEPGRAM_API_KEY` and `GEMINI_API_KEY` are set, loopback otherwise.

**Shared code** (`supabase/functions/_shared/`): `audio.ts` (WAV, IMA ADPCM), `assembler.ts`,
`providers.ts`, `ai_pipeline.ts`, `mode.ts`.

**Firmware 0.3.0-m2:**
- `adpcm.{h,cpp}`: ADPCM upload, byte-identical to the cloud encoder.
- Freshness and replay fixes, interleaved downloads, the `chunkSeq` stale rule, and diagnostics
  (`reset`, `deadChunks`, `skippedStale`).
- Student page: `adpcm.js` and the earphones notice.

**Tools:** `tools/run-sql-tests.mjs` (rolled-back DB tests), `tools/adpcm-check/` (C++ vs TS
encoder), and ADPCM support in `sim-hub.mjs` / `mock-hub.mjs`.

## 4. Tests executed (2026-11-12)

| Suite | Result |
| --- | --- |
| Deno, mocked providers (`deno test supabase/functions/_shared/`): audio/ADPCM, assembler boundaries, Deepgram/Gemini parsing, timeouts, retries, worker stages | **55 passed**, 1 ignored (the live test, opt-in) |
| DB, Milestone 2 (`supabase/tests/m2_pipeline_test.sql`): demand, one job per language, final listener, ordering gate, duplicates, isolation, cost cap, passthrough, flush, retention, permissions | **41/41** |
| DB, Milestone 1 (`hub_api_test.sql`) against the new schema | **31/31** |
| Simulated Hub against the deployed cloud, ADPCM and PCM uploads | **17/17** each |
| Firmware ADPCM encoder (C++, host build) vs cloud encoder | byte-identical on 5 signal types |
| Student page in headless Chrome against the mock Hub (ADPCM) | 26 segments decoded and played; none played twice; no page errors |
| Deployed functions without the secret key | 401 (all three) |
| Firmware compile | see the PR |

Opt-in real-API test (costs under $0.01):

```bash
ELINGO_LIVE_PROVIDER_TESTS=1 DEEPGRAM_API_KEY=... GEMINI_API_KEY=... \
  deno test --allow-env --allow-net supabase/functions/_shared/providers_live_test.ts
```

## 5. Remaining limitations and risks

0. **First real-provider run (2026-11-12, `tools/ai-smoke.mjs`, 33 s English clip, phones on
   af, af and st):**
   - Deepgram transcribed all 7 chunks correctly, and the stitched sentences had no repeated
     or missing words at any boundary.
   - All Gemini calls failed with HTTP 429: the key is on the free tier, limited to 5 requests
     per minute for `gemini-3.8-flash`. A few calls got 503 "high demand".
   - Fixed after this run:
     - Rate-limit retries now wait for the provider's own retry hint, up to 30 s.
     - A sentence that waits too long is now split at its last full stop instead of
       mid-sentence.
   - Needed: a Gemini key on a billing-enabled project.

1. **The Gemini Interactions response shape has not been verified against the real API** (it was
   coded from the docs). Run the live test first. `GEMINI_API=generate_content` is the fallback.
2. Translation quality into st, nso and ss needs native-speaker review.
3. The microphone hardware fault must be fixed before any meaningful AI test.
4. End-to-end latency is estimated at 11–20 s (5 s chunk, sentence wait, translation and TTS) and
   is not yet measured.
5. Deepgram `multi` does not cover Sesotho or Setswana as *source* languages. The lecturer
   should speak English or another supported language.
6. A phone speaker near the Hub feeds back into the microphone. Earphones are required: the
   page says so, but nothing enforces it.
7. The cause of the Hub's reboots is still unknown (now reported).
