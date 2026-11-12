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
| Status LED (common cathode) | Red | 11 | through a resistor (220–330 Ω) |
| Status LED | Green | 12 | through a resistor |
| Status LED | Blue | **13** | **new wire** – through a resistor; common pin to GND |
| Push button | – | 10 | other side to GND (internal pull-up) |

The 16×2 LCD is no longer used and can be removed. Do not use GPIO 26–37 (octal flash/PSRAM)
or 47/48 (1.8 V on this module).

### Verified microphone configuration (do not change without new evidence)

I2S0 master RX, legacy `driver/i2s.h`, 16 kHz, 32-bit slots, LEFT slot, `sample >> 14` to
16-bit, "DSP B" (80 Hz high-pass, 1.5× gain), 5 s chunks of 160,044 bytes.

Evidence: serial logs with SCK=4/WS=5/SD=6 (the old README and diag pins) gave RMS 0 on both
slots; recordings with the wiring above contain real signal. Two measured defects were fixed
without changing that configuration: capture no longer stops between chunks (each restart
caused a ~300 ms INMP441 start-up burst plus an audio gap during upload), and the int16
conversion now saturates instead of wrapping.

## LED and button

| LED | Meaning |
| --- | --- |
| Red → green → blue once | Power-on self-test |
| Off | Ready (idle), or after a session |
| Short red blink every 2 s | No upstream Wi-Fi or cloud: sessions cannot start |
| **Blue solid** | Button pressed: session started, waiting for translation |
| **Green solid** (red, blue off) | Translated audio is arriving |
| Blue or green blinking 1 Hz | Session running but the cloud is unreachable (audio is buffered ~60 s) |
| **Red solid for 5 s, then off** | Session ended by the button |
| Blue double-blink | Setup page open |
| Red fast blink | Hardware fault (microphone or PSRAM) |

Button:

- **Short press**: start a session when ready; end it when running. A second press within 2 s
  is ignored (no accidental start-then-stop).
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
