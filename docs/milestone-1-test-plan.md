# Milestone 1 – acceptance test plan

Status key: **verified** = executed with recorded output; **pending** = needs the physical Hub
and phones. Record results (pass/fail, numbers, serial `status` output) in the PR.

## Already verified (no hardware)

| Check | How | Result |
| --- | --- | --- |
| DB access control, demand counts, idempotency, hub isolation, expiry, session end | `supabase/tests/hub_api_test.sql` on the dev project | 31/31 pass |
| Worker logic (skip with no listeners, one segment per language, retries, dead letter, cleanup) | `deno test supabase/functions/_shared/pipeline_test.ts` | 12/12 pass |
| Full cloud path with the ESP32's exact requests | `node tools/sim-hub.mjs` | 17/17 pass |
| Edge Functions refuse callers without the secret key | curl: no key 401, publishable key 401, secret 200 | pass |
| Supabase security advisors | `supabase db advisors --remote` | 0 errors; 8 expected warnings (Hub RPCs are callable by signed-in users by design; sign-ups disabled) |
| Firmware compiles | `arduino-cli compile` (see PR) | see PR |

## Field data from 2026-11-12 (cloud records of Hub "eLingo Hub 1", 17 sessions)

| Finding | Evidence | Status |
| --- | --- | --- |
| Microphone sends exact zeros for long stretches | e.g. session 619b9263: 71 of 73 chunks RMS 0 / peak 0, with good chunks in between; 10950d69: 7 of 7 | **open, hardware**: firmware 0.2.1 flags it (`NO SIGNAL`, `deadChunks`); fix the wiring |
| Uploads fall behind when phones listen | no listeners: capture→upload ≈ 8 s (5 s chunk + 3 s); 1 language: 17–28 s and growing; 2 languages (c6b4aa39): mean 47 s, max 71 s, 14 of 130 chunks dropped | **partly fixed** in 0.2.1 (downloads no longer starve uploads; stale audio skipped). Root cause is bandwidth: 5.7 KB TCP window → ~30–60 KB/s, vs 32 KB/s up + 32 KB/s down per language. Needs compressed audio (Milestone 2, phase 1) |
| Hub restarts mid-session | capture gaps of 27–76 s with consecutive seqs (resume after reboot); 3 sessions ended `hub_restarted` | **open**: 0.2.1 reports the reset reason |
| Old audio replayed | resume polled from segment 0; idle languages and the Hub buffer could serve minutes-old audio; the page's live-edge join replayed the last heard segment | **fixed** in 0.2.1 (tests 23–26) |

## Physical tests (pending)

Setup: flash firmware + web files, provision the Hub, power it from a USB charger (no laptop),
upstream = phone hotspot, two student phones (A, B).

Before powering on, check the LED wiring: R→GPIO11, G→GPIO12, B→GPIO13, common→GND, with a
series resistor per colour (on the module or added).

| # | Test | Expected | Result |
| --- | --- | --- | --- |
| 1 | Power on from a charger | Red, then green, then blue, each alone for 0.3 s (confirms each channel goes to the right colour). Then a short red blink every 2 s until online, then LED off | |
| 2 | Upstream internet | Serial/`status`: upstream connected, cloud online | |
| 3 | Student network | Phones see **eLingo-Hub**, connect with the AP password | |
| 4 | Page loads locally | Page opens via captive portal or http://192.168.4.1 with mobile data **off** | |
| 5 | Two phones connect | Footer shows "2 of 10 devices connected" | |
| 6 | Start session | Short press → blue solid immediately, red and green off; stays blue while nobody listens | |
| 7 | Same language | A and B choose Afrikaans → both hear the lecturer (loopback) ~10 s delayed; card shows 2 listening | |
| 8 | First audio | LED turns green (blue and red off) at the same moment serial prints the first `[Cloud] Segment … stored`. It does not turn green earlier, e.g. at chunk upload | |
| 9 | Different languages | B switches to Sesotho → af=1, st=1; both keep playing | |
| 10 | One leaves, one stays | A taps Leave → B unaffected; af inactive | |
| 11 | Final listener leaves | B leaves → no new segments generated (`source_chunks.status = skipped_no_listeners`) | |
| 12 | Phone Wi-Fi drop | Turn A's Wi-Fi off 1 min, back on → page reconnects and re-joins by itself | |
| 13 | Upstream drop | Turn hotspot off 30 s → LED stays solid green; page shows "Live · weak link"; back on → queued chunks upload | |
| 14 | Hub reboot mid-session | Reset button → Hub resumes the same session; phones recover | |
| 15 | Duplicate subscriptions | Refresh page 5× → still one listener (same id) | |
| 16 | Expired listener | Close A's browser → removed after ~30 s on the Hub, ~45 s in the cloud | |
| 17 | End session | Second press → red only, immediately; stopwatch 5 s (±0.2 s) → all off. Serial shows `Session … end: ok`. Phones show "The lecturer has ended the session", then "The lecture has ended" | |
| 18 | Button bounce | Double-tap at start → stays blue (second tap ignored, 2 s lockout); press during the 5 s red → ignored, red still ends on time | |
| 18a | Next session | After the red, press again → blue; phone left on the page plays the new lecture without reloading | |
| 18b | Quick restart | Press start within 2–3 s of the red ending → blue at once; serial shows the previous end finishing before the new `Session … started` | |
| 18c | Blue with no listeners | Start with no phone on a language → LED stays blue; choose a language on a phone → green within ~15 s | |
| 19 | Setup page | Hold 5 s → blue double blink; change upstream Wi-Fi from a phone without reflashing | |
| 20 | Long session | 60 min session: `status` every 10 min — free heap stable, `dropped` 0, no resets | |
| 21 | Audio quality | First chunk of a session has no start-up burst; no clicks on loud speech | |
| 22 | No dead microphone | 10 min session: serial never prints `NO SIGNAL` / `microphone sends no data`; `status` → `deadChunks` 0. If it does, re-seat or solder the INMP441 wires (VDD 3.3 V, GND, L/R→GND, SCK 6, WS 5, SD 4) and repeat | |
| 23 | No replay after pause | Phone A pauses 30 s, resumes → hears live audio, never a segment it already heard | |
| 24 | No replay after reboot | Reset the Hub mid-session with A listening → after it resumes, A hears only new audio (serial may show `Segment … skipped: N chunks behind live`), never the lecture from the start | |
| 25 | No stale join | A listens to af, leaves; 2 min later B joins af → B hears nothing until new audio arrives, never the old segment | |
| 26 | Next session | End, start a new session → phones play only the new lecture; nothing from the previous one | |
| 27 | Live lag with 2 languages | A=af, B=st for 10 min: serial `Chunk n uploaded+registered` keeps up (queued stays ≤ 2); clap-to-phone delay recorded | |
| 28 | Reboot cause | After any unexpected restart, `hubs.last_stats.reset` names the cause (crash, brownout, external pin…) | |

Students must use earphones. A phone speaker near the Hub is picked up by the INMP441 and
played to everyone again ~10 s later, which sounds like the audio is looping.

### Measurements to record

- Per chunk: upload + register time (serial `[Cloud] Chunk n uploaded+registered in … ms`).
- Segment download time (serial `[Cloud] Segment …`).
- End-to-end delay lecturer → phone (clap test with a stopwatch); expected ≥ 5 s chunk + upload + processing + poll.
- Free heap / min heap / free PSRAM (`status`) at start and after 60 min.
- Gaps: compare `captured` vs `uploaded` vs `dropped` counters.
