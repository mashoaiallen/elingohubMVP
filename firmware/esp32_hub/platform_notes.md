# Platform notes

## Networking (AP+STA on one radio)

- The soft-AP and the upstream station share one 2.4 GHz radio. The AP always runs on the
  upstream network's channel; when that changes the ESP32 moves the AP and sends a Channel
  Switch Announcement. Phones that ignore CSA reconnect — test with your phones.
- Upstream retries back off from 5 s to 60 s so a missing hotspot does not keep disturbing
  students. Wi-Fi scans (setup page only) also briefly pause the AP.
- The AP does not route to the internet; phones only need the Hub. Android may show "no
  internet"; the page still works because 192.168.4.1 is on the local subnet.
- Soft-AP station limit is 10 with this core (CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM=7 →
  17 − 7). `max_open_sockets` for the web server is 9 of LWIP's 16 sockets, leaving room for
  TLS, DNS and SNTP; idle sockets are recycled (LRU purge).
- Captive portal: a DNS server answers every name with 192.168.4.1 and unknown URLs redirect
  to the student page, so phones' connectivity checks open it automatically.

## Cloud connection

- HTTPS with the Mozilla CA bundle compiled into the core (`_binary_x509_crt_bundle_start`).
- One keep-alive TLS connection, reused for every request from the cloud task.
- The Hub signs in as its own Supabase Auth user; refresh tokens rotate (the newest is kept
  in RAM; after a reboot it signs in with the stored password).

## Memory (16 MB PSRAM)

| Buffer | Size |
| --- | --- |
| Capture queue: 12 × 160 KB chunks | ~1.9 MB (~60 s of audio while offline) |
| Segment store (all languages) | ≤ 6 MB, ≤ 12 segments per language |
| Download buffers / TLS records | allocated in PSRAM (> 4 KB allocations) |

Task stacks are in internal RAM: cloud 16 KB, audio 6 KB, httpd 8 KB. Measure with the
`status` serial command (free heap, minimum heap, free PSRAM) during long sessions.

## Legacy I2S driver

The verified microphone setup uses `driver/i2s.h`, which ESP-IDF 5 marks deprecated (compile
warnings are expected). It is kept on purpose until a hardware-verified port to `i2s_std` exists.

## LittleFS

The student page is in `data/` and flashed as a LittleFS image to the `spiffs` partition at
0x910000 (size 0x16E0000) by `tools/flash-web.mjs`. Without it the Hub serves a short fallback
page explaining how to flash it.
