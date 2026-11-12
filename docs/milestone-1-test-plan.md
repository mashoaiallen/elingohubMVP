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

## Physical tests (pending)

Setup: flash firmware + web files, provision the Hub, power it from a USB charger (no laptop),
upstream = phone hotspot, two student phones (A, B).

| # | Test | Expected | Result |
| --- | --- | --- | --- |
| 1 | Power on from a charger | R→G→B sweep, then red blink until online, then LED off | |
| 2 | Upstream internet | Serial/`status`: upstream connected, cloud online | |
| 3 | Student network | Phones see **eLingo-Hub**, connect with the AP password | |
| 4 | Page loads locally | Page opens via captive portal or http://192.168.4.1 with mobile data **off** | |
| 5 | Two phones connect | Footer shows "2 of 10 devices connected" | |
| 6 | Start session | Short press → blue solid | |
| 7 | Same language | A and B choose Afrikaans → both hear the lecturer (loopback) ~10 s delayed; card shows 2 listening | |
| 8 | First audio | LED turns green when the first segment arrives | |
| 9 | Different languages | B switches to Sesotho → af=1, st=1; both keep playing | |
| 10 | One leaves, one stays | A taps Leave → B unaffected; af inactive | |
| 11 | Final listener leaves | B leaves → no new segments generated (`source_chunks.status = skipped_no_listeners`) | |
| 12 | Phone Wi-Fi drop | Turn A's Wi-Fi off 1 min, back on → page reconnects and re-joins by itself | |
| 13 | Upstream drop | Turn hotspot off 30 s → LED blinks (blue/green), students keep the page; back on → queued chunks upload, LED solid | |
| 14 | Hub reboot mid-session | Reset button → Hub resumes the same session; phones recover | |
| 15 | Duplicate subscriptions | Refresh page 5× → still one listener (same id) | |
| 16 | Expired listener | Close A's browser → removed after ~30 s on the Hub, ~45 s in the cloud | |
| 17 | End session | Short press → red 5 s → off; page shows "Waiting for the lecture" | |
| 18 | Button bounce | Double-tap → second tap ignored (2 s lockout) | |
| 19 | Setup page | Hold 5 s → blue double blink; change upstream Wi-Fi from a phone without reflashing | |
| 20 | Long session | 60 min session: `status` every 10 min — free heap stable, `dropped` 0, no resets | |
| 21 | Audio quality | First chunk of a session has no start-up burst; no clicks on loud speech | |

### Measurements to record

- Per chunk: upload + register time (serial `[Cloud] Chunk n uploaded+registered in … ms`).
- Segment download time (serial `[Cloud] Segment …`).
- End-to-end delay lecturer → phone (clap test with a stopwatch); expected ≥ 5 s chunk + upload + processing + poll.
- Free heap / min heap / free PSRAM (`status`) at start and after 60 min.
- Gaps: compare `captured` vs `uploaded` vs `dropped` counters.
