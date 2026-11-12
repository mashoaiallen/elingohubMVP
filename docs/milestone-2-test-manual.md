# Milestone 2 – physical testing manual

Record every result in the PR: pass/fail, numbers, and the serial log excerpts asked for.
Milestone 2 is complete only when every test in section 3 passes on real hardware.

## 1. Preparation (once)

1. **Fix the microphone wiring.** On 2026-11-12, whole sessions recorded pure digital silence.
   Solder or firmly re-seat the five INMP441 wires:

   | INMP441 | ESP32-S3 |
   | --- | --- |
   | VDD | 3.3 V (measure it) |
   | GND | GND |
   | L/R | GND |
   | SCK | GPIO6 |
   | WS | GPIO5 |
   | SD | GPIO4 |

   Use short wires and no breadboard if possible.
2. **Provider keys** (only you handle them; never commit them or put them in the firmware).
   The Gemini key must belong to a Google Cloud project **with billing enabled** (paid tier).
   The free tier allows 5 requests/min for `gemini-3.8-flash`, which a live lecture exceeds
   within seconds (measured 2026-11-12), and Google may use free-tier content to improve its
   products.
   ```bash
   supabase secrets set DEEPGRAM_API_KEY=... GEMINI_API_KEY=...
   ```
   The workers switch from loopback to AI by themselves (`PIPELINE_MODE` defaults to `auto`).
   Check this by calling the function with the secret key: it reports `"mode":"ai"`.
3. **Cloud-only end-to-end check** (no Hub needed, about $0.01): stream a speech recording
   through the deployed pipeline as a simulated Hub with phones on af, af and st:
   ```bash
   node tools/ai-smoke.mjs --creds tools/.hub-credentials-simulated-hub-dev.json --wav speech-16k-mono.wav
   ```
   Expected: af and st segments come back, and the WAVs written to `ai-smoke-out/` play the
   translation. It prints the delay of each segment after its chunk was uploaded.

   **Live provider check** (costs under $0.01): run the command in
   [milestone-2-report.md §4](milestone-2-report.md#4-tests-executed-2026-11-12). Expected:
   - an Afrikaans translation;
   - TTS audio at 16000 Hz from both TTS models;
   - Deepgram words.

   If the translation or TTS step fails with a response-shape error, set
   `supabase secrets set GEMINI_API=generate_content` and run it again.
4. **Flash** firmware `0.3.0-m2` and the web files (`node tools/flash-web.mjs`), as in the
   firmware README. Serial at power-on must show `eLingo Hub firmware 0.3.0-m2`.
5. **Mic check:** start a session and speak. Every `[Audio] Chunk n ready` line must show RMS in
   the hundreds or more, and never `NO SIGNAL`. Do not continue until this passes for 5 minutes.

## 2. Setup for each test run

The Hub is powered from a USB charger (no laptop) and the upstream is a phone hotspot. Phones A
and B are on the **eLingo-Hub** Wi-Fi with **mobile data off**, wearing earphones. The lecturer
speaks normally at 1–2 m from the Hub.

To read the cloud's view of a session, run:
```bash
supabase db query --remote --read-only "select * from public.session_summary order by started_at desc limit 1"
supabase db query --remote --read-only "select operation, model, language_code, unit_type, requests, errors, retries, units, est_cost_usd, avg_latency_ms from public.session_usage where session_id = '<id>'"
```

## 3. Tests

| # | Test | Steps | Expected | Result |
| --- | --- | --- | --- | --- |
| P1 | Same language | A and B choose Afrikaans, then Start listening | Both hear Afrikaans speech of what the lecturer said. `translation_jobs` has **one** af job per utterance (not two). LED blue, then green | |
| P2 | Different languages | B switches to Sesotho | A hears Afrikaans and B hears Sesotho, both continuously. Jobs exist for af and st only | |
| P3 | Lecturer's language | B chooses English while the lecturer speaks English | B hears the lecturer's own voice. Usage shows no Gemini call for en | |
| P4 | One student leaves | A taps Leave | B unaffected. No new af jobs after about 15 s (`skipped_no_listeners` at most for jobs already queued) | |
| P5 | Final listener leaves | B also leaves; keep talking 1 min | Chunks become `skipped_no_listeners`. **No Deepgram or Gemini usage rows** during that minute | |
| P6 | Rejoin | A chooses af again | Translation resumes within ~15–20 s. A never hears audio from before it left | |
| P7 | Phone Wi-Fi drop | A turns Wi-Fi off for 1 min, then on | The page reconnects and re-joins by itself. Footer still counts A once. No replay of old audio | |
| P8 | Upstream drop | Turn the hotspot off for 30 s, then on | Page shows "Live · weak link". After recovery, audio resumes near live. Audio more than 40 s behind is skipped (serial: `skipped: N chunks behind live`), not played late | |
| P9 | Ordering | 5 min with two languages | Sentences play in the order spoken: no overlap, no repeats, no gaps beyond natural pauses | |
| P10 | Accuracy | Lecturer reads a prepared technical paragraph | Afrikaans translation is complete (no summarising) and keeps the technical terms. A native speaker rates it | |
| P11 | Latency | Lecturer says a short distinct sentence ("Number seven."), stopwatch from end of speech to start of the translated audio on the phone. 10 repetitions | Record min / median / max. Also record `avg_utterance_to_audio_ms` from `session_summary` | |
| P12 | Session end | Press the button mid-sentence | Red for 5 s. Phones show "lecture ended". Speech from the last ~10 s before the press is not delivered: the Hub stops fetching at the press (known limitation) | |
| P13 | Next session | Press again | New session; phones play only the new lecture | |
| P14 | Sustained lecture | 45–60 min, two languages | No resets (`hubs.last_stats.reset` unchanged), `dropped` 0, `deadChunks` 0, free heap stable. Translation continues to the end | |
| P15 | Costs | After P14 | Record `session_summary.cost_usd` and `session_usage`. Compare with the estimate in the report (~$2.50/h for af + st) | |
| P16 | Hub reboot | Reset the Hub mid-session | The Hub resumes the session, phones recover, and nobody hears the lecture again from the start | |
| P17 | Translation unavailable | Free-tier Gemini key, or temporarily set `GEMINI_API_KEY` to an invalid value | Students still hear every sentence, as the lecturer's original voice. The page shows "Original audio · translation unavailable". Serial shows `original audio`. `session_summary.segments_original_fallback` > 0 | |

## 4. What to send back

- The table above, filled in.
- Serial log excerpts: boot banner, 2 minutes of chunks/segments, any `WARNING`/`failed` lines.
- The `session_summary` and `session_usage` output for P14.
- Latency numbers from P11.
