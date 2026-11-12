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

- **Milestone 1 – standalone Hub + Supabase (this branch).** Cloud runs a *loopback* pipeline
  (source audio copied to each active language) to prove the full data path without AI cost.
- **Milestone 2 – Deepgram + Gemini** replaces loopback with transcription, translation and
  speech generation.

Supported target languages (Deepgram Nova-3 + Gemini 3.8 Flash TTS, checked 2026-11-12):
English, Afrikaans, Sesotho, Sepedi, siSwati. isiZulu, isiXhosa and Setswana are not offered
because Gemini TTS does not support them.

## Repository layout

| Path | Contents |
| --- | --- |
| `firmware/esp32_hub/` | Hub firmware ([README](firmware/esp32_hub/README.md)); `data/` = student page |
| `firmware/inmp441_diag/` | Stand-alone microphone diagnostic |
| `supabase/migrations/` | Database schema, RLS, RPCs, queue, cron, storage policies |
| `supabase/functions/` | Edge Functions: `process-chunk` (queue worker), `cleanup` (retention) |
| `supabase/tests/` | Database tests |
| `tools/` | Provisioning, simulated Hub, web-file flasher (Node ≥ 20, no dependencies) |

## Cloud setup (once per Supabase project)

```bash
supabase link --project-ref <ref>
supabase db push --remote                       # schema
supabase functions deploy process-chunk cleanup --use-api
supabase config push --remote                   # disables public sign-ups
# Vault secrets used by pg_net to wake the worker (names elingo_project_url, elingo_worker_key):
#   select vault.create_secret('https://<ref>.supabase.co', 'elingo_project_url');
#   select vault.create_secret('<secret key>', 'elingo_worker_key');
```

`tools/.env` (gitignored) holds `SUPABASE_URL`, `SUPABASE_PUBLISHABLE_KEY`,
`SUPABASE_SECRET_KEY` for the tools. Provider keys for Milestone 2 go in Supabase secrets only.

## Tests

```bash
supabase db query --remote -f supabase/tests/hub_api_test.sql      # 31 DB checks, rolled back
npx deno test supabase/functions/_shared/pipeline_test.ts          # worker logic, 12 tests
node tools/sim-hub.mjs --creds tools/.hub-credentials-<hub>.json  # end-to-end cloud path, 17 checks
```

Run the DB tests and the simulated Hub against a non-production project.

## Security notes

- Students never reach Supabase; only provisioned Hubs can sign in (sign-ups disabled).
- All tables have RLS with no direct access; Hubs use ownership-checked RPCs and their own
  Storage folder; workers use the secret key, which never leaves Supabase.
- Lecture audio is private and temporary: source audio is deleted 10 minutes after processing,
  translated audio 15 minutes after it is ready.
- Git history before this branch contains Wi-Fi credentials and recordings (the repository is
  public). Rotate that Wi-Fi password.
