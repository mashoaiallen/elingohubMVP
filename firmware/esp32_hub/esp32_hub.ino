// eLingo Hub firmware - standalone ESP32-S3 Hub (Milestone 1).
// See README.md for wiring, LED/button behaviour, provisioning and flashing.

#include <LittleFS.h>

#include "audio_manager.h"
#include "button_manager.h"
#include "cloud_sync.h"
#include "config.h"
#include "led_manager.h"
#include "listener_registry.h"
#include "local_server.h"
#include "segment_store.h"
#include "session_manager.h"
#include "settings.h"
#include "state_machine.h"
#include "wifi_manager.h"

SettingsStore settings;
LedManager led;
ButtonManager button;
WifiManager wifi;
AudioManager audio;
SessionManager session;
ListenerRegistry listeners;
SegmentStore segments;
CloudSync cloud(settings, wifi, audio, listeners, segments, session);
LocalServer server(settings, wifi, listeners, segments, session);
StateMachine machine(settings, wifi, led, button, audio, session, listeners, segments, cloud, server);

namespace {

String consoleLine;

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  status                  hub state, memory, network"));
  Serial.println(F("  show                    stored settings (secrets masked)"));
  Serial.println(F("  set <key> <value>       keys: sta_ssid sta_pass ap_pass setup_pin supabase_url"));
  Serial.println(F("                          publishable_key hub_email hub_password"));
  Serial.println(F("  setup                   toggle the setup page (same as a 5 s button press)"));
  Serial.println(F("  factory-reset           clear NVS and re-seed from secrets.h"));
  Serial.println(F("  reboot"));
}

void printStatus() {
  HubSnapshot s = session.snapshot();
  Serial.printf("state=%s session=%s translating=%d degraded=%d cloud=%d upstream=%d(%s, %d dBm, ch %u)\n",
                machine.stateName(), s.sessionActive ? s.sessionId : "-", s.translating, s.degraded,
                cloud.online(), wifi.staConnected(), wifi.staSsid().c_str(), wifi.staRssi(), wifi.channel());
  Serial.printf("students: %u on Wi-Fi, %u listening | chunks: captured %lu uploaded %lu queued %lu dropped %lu\n",
                wifi.stationCount(), listeners.count(), static_cast<unsigned long>(audio.capturedChunks()),
                static_cast<unsigned long>(cloud.uploadedChunks()), static_cast<unsigned long>(audio.queuedChunks()),
                static_cast<unsigned long>(audio.droppedChunks()));
  Serial.printf("memory: heap %lu (min %lu) psram %lu/%lu | segments %lu (%lu bytes) | fw %s\n",
                static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
                static_cast<unsigned long>(ESP.getFreePsram()), static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(segments.segmentCount()), static_cast<unsigned long>(segments.bytesUsed()),
                Config::FIRMWARE_VERSION);
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  if (line == "help") {
    printHelp();
  } else if (line == "status") {
    printStatus();
  } else if (line == "show") {
    settings.printMasked(Serial);
  } else if (line == "setup") {
    machine.toggleSetupMode();
  } else if (line == "reboot") {
    ESP.restart();
  } else if (line == "factory-reset") {
    settings.factoryReset();
    Serial.println("Settings reset; rebooting");
    delay(200);
    ESP.restart();
  } else if (line.startsWith("set ")) {
    String rest = line.substring(4);
    int space = rest.indexOf(' ');
    String key = space < 0 ? rest : rest.substring(0, space);
    String value = space < 0 ? String() : rest.substring(space + 1);
    if (!settings.setField(key, value)) {
      Serial.println("Unknown key or invalid value (type help)");
      return;
    }
    Serial.printf("Saved %s\n", key.c_str());
    HubSettings s = settings.snapshot();
    if (key == "sta_ssid" || key == "sta_pass") {
      wifi.setStaCredentials(s.staSsid, s.staPassword);
    } else if (key == "ap_pass") {
      wifi.restartAp(s.apPassword);
    } else if (key != "setup_pin") {
      cloud.reloadSettings();
    }
  } else {
    Serial.println("Unknown command (type help)");
  }
}

void pollConsole() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      handleCommand(consoleLine);
      consoleLine = "";
    } else if (consoleLine.length() < 300) {
      consoleLine += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\neLingo Hub firmware %s\n", Config::FIRMWARE_VERSION);

  led.begin();
  button.begin();
  settings.begin();

  bool fsMounted = LittleFS.begin(false);
  if (!fsMounted) {
    Serial.println("[FS] LittleFS not mounted - flash the web files (tools/flash-web.mjs)");
  }

  HubSettings s = settings.snapshot();
  wifi.begin(s.staSsid, s.staPassword, s.apPassword);
  bool audioOk = audio.begin();
  server.begin(fsMounted);
  cloud.begin();
  machine.begin(!audioOk);

  if (!s.cloudConfigured()) {
    Serial.println("[Hub] Cloud credentials missing: run tools/provision-hub.mjs and use 'set' (type help)");
  }
  if (s.setupPin.length() == 0) {
    Serial.println("[Hub] No setup PIN: use 'set setup_pin <digits>' to enable the setup page");
  }
  printHelp();
}

void loop() {
  machine.update();
  wifi.update(millis());
  pollConsole();
  delay(5);
}
