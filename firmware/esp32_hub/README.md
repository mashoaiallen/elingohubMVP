# eLingo Hub firmware (ESP32-S3)

Standalone Hub: captures the lecturer with an INMP441, hosts the student Wi-Fi network and web
page, and exchanges audio with Supabase. No laptop is needed to operate it.

## Hardware and wiring

Board: Waveshare ESP32-S3-DEV-KIT-N32R16V (32 MB octal flash, 16 MB octal PSRAM).

| Part | Signal | GPIO | Notes |
| --- | --- | --- | --- |
| INMP441 | SCK / BCLK | 6 | verified |
| INMP441 | WS / LRCLK | 5 | verified |
| INMP441 | SD / DOUT | 4 | verified; L/R pin to GND (LEFT slot) |
| RGB LED module (common cathode) | R | 11 | same pin as the original red LED |
| RGB LED module | G | 12 | same pin as the original green LED |
| RGB LED module | B | **13** | **new wire** |
| RGB LED module | common (–, GND) | GND | the long leg on a bare RGB LED |
| Push button | – | **9** | other side to GND (internal pull-up). Was GPIO10 up to firmware 0.3.0-m2; wire it straight to the pin, not through a breadboard rail |

The LED is driven **active-HIGH** (GPIO HIGH = colour on). Each colour needs its own series
resistor. Many modules already have them; look for three small resistors on the module's
board. If yours has none, use about 220 Ω for R and 100 Ω for G and B. Green and blue need
about 3 V, which leaves little headroom on a 3.3 V pin. Keep each channel under 20 mA.

Why these pins: they are 3.3 V I/O and not strapping pins (0, 3, 45, 46). They are not used by
the octal flash/PSRAM (26–37), the USB port (19, 20), UART0 (43, 44), the microphone (4, 5, 6)
or the button (9). The board's own WS2812 LED is on GPIO38. The 16×2 LCD is no longer used and
can be removed; its pins 7 and 15 are now free. Do not use GPIO 47/48: they are 1.8 V on this
module.

### Verified microphone configuration (do not change without new evidence)

I2S0 master RX, legacy `driver/i2s.h`, 16 kHz, 32-bit slots, LEFT slot, `sample >> 14` to
16-bit, "DSP B" (80 Hz high-pass, 1.5× gain), 5 s chunks of 160,044 bytes.

Evidence: serial logs with SCK=4/WS=5/SD=6 (the old README and diag pins) gave RMS 0 on both
slots; recordings with the wiring above contain real signal. Two measured defects were fixed
without changing that configuration: capture no longer stops between chunks (each restart
caused a ~300 ms INMP441 start-up burst plus an audio gap during upload), and the int16
conversion now saturates instead of wrapping.

## Audio upload and delivery (firmware 0.3.0)

Capture is unchanged (16 kHz, 32-bit slots, `>> 14`, LEFT slot). Each finished 5 s chunk is
encoded as **IMA ADPCM WAV** (40 KB instead of 160 KB PCM) before upload: this core's lwIP has
a 5.7 KB TCP window, and PCM fell 47–71 s behind once two languages were listening. Translated
audio also arrives as ADPCM (~8 KB/s per language) and the student page decodes it
(`data/adpcm.js`). The encoder (`adpcm.cpp`) is byte-identical to the cloud's (`tools/adpcm-check`).

The cloud task alternates uploads and downloads (at most one download per pass). It skips any
translated segment more than 8 chunks (40 s) behind the microphone. The Hub stops offering
segments older than 60 s. Phones never play a segment twice, and they only join at a segment
less than 15 s old.

Diagnostics in `status` and in `hubs.last_stats`: `reset` (why the Hub last restarted),
`deadChunks` (chunks of pure digital silence, meaning a microphone wiring fault) and
`skippedStale`.

## LED and button

A session shows exactly three states, each solid and one colour at a time:

| Session state | LED | Starts when | Ends when |
| --- | --- | --- | --- |
| **Session started** | **blue** only | first press (Hub ready) | first translated audio stored on the Hub |
| **Translation received** | **green** only | the Hub has downloaded and stored the first playable translated segment, not when processing starts | session ends |
| **Session ended** | **red** only, 5 s | second press (or the cloud ends the session) | 5 s later: all off, Hub idle and ready for the next press |

Green needs a phone to be listening. The Hub only fetches languages that a connected phone has
chosen, and the cloud makes no audio for languages nobody listens to. With no phone listening,
the LED stays blue for the whole session. In Milestone 1 the "translated" audio is the
loopback copy of the lecturer's audio.

Outside a session the LED is off, apart from these diagnostics, none of which looks like the
three session states:

| LED | Meaning |
| --- | --- |
| Red → green → blue once (0.9 s) | Power-on self-test |
| Short red blink every 2 s | No upstream Wi-Fi or cloud: the button cannot start a session |
| Blue double-blink | Setup page open (5 s press) |
| Red fast blink (5 Hz) | Hardware fault (microphone or PSRAM) |

If a start fails (the cloud is unreachable for 20 s), blue goes off again and the Hub stays
idle. A weak cloud link during a session does not change the LED; students see "weak link" on
their page.

Button:

- **Short press**: start a session when idle; end it when running. Presses within 2 s of the
  last one are ignored, so a double-tap cannot start and stop. Presses while the Hub is still
  starting (blue, before the cloud confirms) or during the 5 s red are ignored too.
- If you press start within a few seconds of an end, the Hub turns blue immediately. It waits
  until the last audio of the previous session has uploaded (at most 10 s) before starting the
  new one.
- **On end**: capture stops, the remaining audio is uploaded (at most 10 s), the session is
  closed in the cloud, and the Hub's stored segments are cleared. Phones show "The lecturer
  has ended the session", then "The lecture has ended". Phones that stay on the page start the
  next lecture from its live edge.
- **Long press (5 s)**: open/close the setup page for 10 minutes. Saved settings are never deleted.

After a reboot during a lecture the Hub resumes the same session (if it was active within the
last 2 minutes); students' pages reconnect on their own.

## Students

1. Join the Wi-Fi network **eLingo-Hub** (password from the lecturer).
2. The page opens by itself on most phones; otherwise open **http://192.168.4.1**.
3. Choose a language, tap **Start listening**, keep the page open. No mobile data needed.

Up to **10 phones** per Hub (soft-AP limit of this ESP32 core build). Students who pick the same
language share one generated stream; each phone has its own playback position.

## Build and flash

Arduino CLI (or IDE) with ESP32 core **3.3.11** and the **ArduinoJson 7** library
(LiquidCrystal_I2C is no longer needed).

```bash
FQBN="esp32:esp32:esp32s3-octal:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=32M,PartitionScheme=app5M_little24M_32MB,PSRAM=opi,FlashMode=opi"
arduino-cli compile --fqbn "$FQBN" firmware/esp32_hub
arduino-cli upload  --fqbn "$FQBN" -p COM5 firmware/esp32_hub
node tools/flash-web.mjs --port COM5        # student page -> LittleFS (once, and after page changes)
arduino-cli monitor -p COM5 -c baudrate=115200
```

## Configuration (no source edits per location)

Settings live in NVS. On first boot each empty field is seeded from `secrets.h` (gitignored;
copy `secrets.example.h`).

1. Cloud identity, once per Hub, from a laptop:
   `node tools/provision-hub.mjs --name "Lecture Hall 1" --write-secrets`
   (or type the values on the serial console, below).
2. Upstream Wi-Fi: `secrets.h`, the serial console, or the **setup page**: hold the button 5 s,
   connect a phone to eLingo-Hub, open `http://192.168.4.1/setup.html`, enter the setup PIN,
   scan, pick the network, save. The Hub reconnects without a reboot.

Serial console (115200 baud): `help`, `status`, `show`, `set <key> <value>`, `setup`,
`factory-reset`, `reboot`. Keys: `sta_ssid sta_pass ap_pass setup_pin supabase_url
publishable_key hub_email hub_password`.

## Architecture

| Task (core) | Module | Job |
| --- | --- | --- |
| audio (1, prio 10) | `audio_manager` | Continuous I2S capture → 5 s WAV chunks → PSRAM queue (12 deep) |
| cloud (0, prio 3) | `cloud_sync`, `cloud_client` | HTTPS to Supabase: upload + register chunks, presence sync, poll + download segments |
| httpd (0) | `local_server` | Student page + API, captive portal, setup page |
| loop (1) | `state_machine`, `button_manager`, `led_manager`, `wifi_manager` | States, button, LED, Wi-Fi |

Shared state: `session_manager` (session + cloud status), `listener_registry` (phones and
their language, 30 s heartbeat expiry), `segment_store` (recent translated audio per language,
reference-counted, ≤ 6 MB PSRAM).
