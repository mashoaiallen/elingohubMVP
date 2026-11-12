# eLingo Hub

A self-contained classroom Hub: an ESP32-S3 captures the lecturer, students connect their phones
to the Hub's own Wi-Fi, choose a language and listen to translated speech. Translation is
demand-driven: one shared stream per language that someone is listening to, and no work at all
for languages nobody selected.

```
INMP441 → ESP32-S3 Hub ──HTTPS──▶ Supabase (Postgres + RLS, Storage, Queues, Edge Functions, Realtime)
            │  ▲                         │ Milestone 2: Deepgram (speech-to-text) → Gemini (translate + TTS)
   eLingo-Hub Wi-Fi                      ▼
            ▼                     translated audio per active language
   students' phones (http://192.168.4.1)
```

## Status

- **Milestone 1 – standalone Hub + Supabase.** Loopback pipeline (source audio copied to each
  active language). Still used automatically while the provider keys are not set.
- **Milestone 2 – Deepgram + Gemini (this branch).** Transcription (Deepgram Nova-3), translation
  (Gemini 3.8 Flash) and speech (Gemini 3.8 Flash / Flash-Lite TTS), one shared stream per
  active language. Implemented and tested automatically; physical validation pending. See
  [docs/milestone-2-report.md](docs/milestone-2-report.md) and the
  [physical testing manual](docs/milestone-2-test-manual.md).

Supported target languages (Deepgram Nova-3 + Gemini 3.8 Flash TTS, checked 2026-11-12):
English, Afrikaans, Sesotho, Sepedi, siSwati. isiZulu, isiXhosa and Setswana are not offered
because Gemini TTS does not support them.

## Repository layout

| Path | Contents |
| --- | --- |
| `firmware/esp32_hub/` | Hub firmware ([README](firmware/esp32_hub/README.md)); `data/` = student page |
| `firmware/inmp441_diag/` | Stand-alone microphone diagnostic |
| `supabase/migrations/` | Database schema, RLS, RPCs, queue, cron, storage policies |
| `supabase/functions/` | Edge Functions: `process-chunk` (speech-to-text stage), `process-translation` (translation + TTS), `cleanup` (retention) |
| `supabase/tests/` | Database tests |
| `tools/` | Provisioning, simulated Hub, web-file flasher (Node ≥ 20, no dependencies) |

## Cloud setup (once per Supabase project)

```bash
supabase link --project-ref <ref>
supabase db push --remote                       # schema
supabase functions deploy process-chunk process-translation cleanup --use-api
supabase secrets set DEEPGRAM_API_KEY=... GEMINI_API_KEY=...   # enables the AI pipeline
supabase config push --remote                   # disables public sign-ups
# Vault secrets used by pg_net to wake the worker (names elingo_project_url, elingo_worker_key):
#   select vault.create_secret('https://<ref>.supabase.co', 'elingo_project_url');
#   select vault.create_secret('<secret key>', 'elingo_worker_key');
```

`tools/.env` (gitignored) holds `SUPABASE_URL`, `SUPABASE_PUBLISHABLE_KEY`,
`SUPABASE_SECRET_KEY` for the tools. Provider keys go in Supabase secrets only (never in the
firmware, the student page or this repository). Optional secrets: `PIPELINE_MODE`
(`auto` default / `ai` / `loopback`), `GEMINI_API` (`interactions` default / `generate_content`).
Models, lag limits, the session cost cap and text retention are rows in `public.app_settings`.

## Tests

```bash
node tools/run-sql-tests.mjs supabase/tests/m2_pipeline_test.sql supabase/tests/hub_api_test.sql  # 41 + 31 DB checks, rolled back
deno test supabase/functions/_shared/                               # 55 worker/codec/provider tests, mocked providers
node tools/sim-hub.mjs --creds tools/.hub-credentials-<hub>.json   # end-to-end cloud path, 17 checks (--pcm for 0.2.x firmware)
g++ -O2 -I firmware/esp32_hub tools/adpcm-check/main.cpp firmware/esp32_hub/adpcm.cpp -o adpcm-encode   && deno run -A tools/adpcm-check/check.ts ./adpcm-encode           # Hub encoder == cloud encoder
# Real provider calls (opt-in, < $0.01): see docs/milestone-2-report.md section 4
```

Run the DB tests and the simulated Hub against a non-production project.

## Security notes

- Students never reach Supabase; only provisioned Hubs can sign in (sign-ups disabled).
- All tables have RLS with no direct access; Hubs use ownership-checked RPCs and their own
  Storage folder; workers use the secret key, which never leaves Supabase.
- Lecture audio is private and temporary: source audio is deleted 10 minutes after processing,
  translated audio 15 minutes after it is ready. Transcripts and translations are erased after
  24 hours, and Gemini is called with `store: false`.
- Git history before this branch contains Wi-Fi credentials and recordings (the repository is
  public). Rotate that Wi-Fi password.
