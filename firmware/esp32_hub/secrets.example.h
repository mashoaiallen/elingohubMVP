// Copy to secrets.h (gitignored) and fill in. Values seed the Hub's NVS settings: a field is
// written from here only while the stored value is empty, so later changes made from the
// setup page or the serial console are kept. Run "factory-reset" on the serial console to
// re-seed everything.
#pragma once

// Upstream internet (phone hotspot or router).
#define ELINGO_DEFAULT_STA_SSID ""
#define ELINGO_DEFAULT_STA_PASSWORD ""

// Student network "eLingo-Hub": at least 8 characters for WPA2, or "" for an open network.
#define ELINGO_DEFAULT_AP_PASSWORD ""

// PIN for the owner setup page (reachable only after a 5-second button press).
#define ELINGO_DEFAULT_SETUP_PIN ""

// Cloud credentials printed by: node tools/provision-hub.mjs --name "<hub name>"
#define ELINGO_SUPABASE_URL ""
#define ELINGO_SUPABASE_PUBLISHABLE_KEY ""
#define ELINGO_HUB_EMAIL ""
#define ELINGO_HUB_PASSWORD ""
