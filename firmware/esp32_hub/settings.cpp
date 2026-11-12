#include "settings.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef ELINGO_DEFAULT_STA_SSID
#define ELINGO_DEFAULT_STA_SSID ""
#endif
#ifndef ELINGO_DEFAULT_STA_PASSWORD
#define ELINGO_DEFAULT_STA_PASSWORD ""
#endif
#ifndef ELINGO_DEFAULT_AP_PASSWORD
#define ELINGO_DEFAULT_AP_PASSWORD ""
#endif
#ifndef ELINGO_DEFAULT_SETUP_PIN
#define ELINGO_DEFAULT_SETUP_PIN ""
#endif
#ifndef ELINGO_SUPABASE_URL
#define ELINGO_SUPABASE_URL ""
#endif
#ifndef ELINGO_SUPABASE_PUBLISHABLE_KEY
#define ELINGO_SUPABASE_PUBLISHABLE_KEY ""
#endif
#ifndef ELINGO_HUB_EMAIL
#define ELINGO_HUB_EMAIL ""
#endif
#ifndef ELINGO_HUB_PASSWORD
#define ELINGO_HUB_PASSWORD ""
#endif

namespace {

constexpr const char *NVS_NAMESPACE = "elingo";

struct FieldSpec {
  const char *nvsKey;      // <= 15 chars
  const char *consoleKey;  // serial console name
  String HubSettings::*member;
  const char *seed;
  bool secret;
};

const FieldSpec FIELDS[] = {
    {"sta_ssid", "sta_ssid", &HubSettings::staSsid, ELINGO_DEFAULT_STA_SSID, false},
    {"sta_pass", "sta_pass", &HubSettings::staPassword, ELINGO_DEFAULT_STA_PASSWORD, true},
    {"ap_pass", "ap_pass", &HubSettings::apPassword, ELINGO_DEFAULT_AP_PASSWORD, true},
    {"setup_pin", "setup_pin", &HubSettings::setupPin, ELINGO_DEFAULT_SETUP_PIN, true},
    {"sb_url", "supabase_url", &HubSettings::supabaseUrl, ELINGO_SUPABASE_URL, false},
    {"sb_pubkey", "publishable_key", &HubSettings::publishableKey, ELINGO_SUPABASE_PUBLISHABLE_KEY, false},
    {"hub_email", "hub_email", &HubSettings::hubEmail, ELINGO_HUB_EMAIL, false},
    {"hub_pass", "hub_password", &HubSettings::hubPassword, ELINGO_HUB_PASSWORD, true},
};

bool validApPassword(const String &value) {
  return value.length() == 0 || (value.length() >= 8 && value.length() <= 63);
}

String mask(const String &value) {
  if (value.length() == 0) {
    return "(empty)";
  }
  return String("(set, ") + value.length() + " chars)";
}

}  // namespace

SettingsStore::SettingsStore() : mutex_(xSemaphoreCreateMutex()) {}

bool SettingsStore::begin() {
  if (!prefs_.begin(NVS_NAMESPACE, false)) {
    Serial.println("[Settings] NVS open failed");
    return false;
  }
  load();
  seedEmptyFieldsFromSecrets();
  return true;
}

void SettingsStore::load() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (const FieldSpec &f : FIELDS) {
    settings_.*(f.member) = prefs_.getString(f.nvsKey, "");
  }
  xSemaphoreGive(mutex_);
}

void SettingsStore::seedEmptyFieldsFromSecrets() {
  for (const FieldSpec &f : FIELDS) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool empty = (settings_.*(f.member)).length() == 0;
    xSemaphoreGive(mutex_);
    if (empty && f.seed[0] != '\0') {
      writeString(f.nvsKey, String(f.seed));
      xSemaphoreTake(mutex_, portMAX_DELAY);
      settings_.*(f.member) = String(f.seed);
      xSemaphoreGive(mutex_);
      Serial.printf("[Settings] Seeded %s from secrets.h\n", f.consoleKey);
    }
  }
}

HubSettings SettingsStore::snapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  HubSettings copy = settings_;
  xSemaphoreGive(mutex_);
  return copy;
}

bool SettingsStore::writeString(const char *key, const String &value) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  size_t written = prefs_.putString(key, value);
  xSemaphoreGive(mutex_);
  return written == value.length();
}

bool SettingsStore::setWifi(const String &ssid, const String &password) {
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63) {
    return false;
  }
  bool ok = writeString("sta_ssid", ssid) && writeString("sta_pass", password);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  settings_.staSsid = ssid;
  settings_.staPassword = password;
  xSemaphoreGive(mutex_);
  return ok;
}

bool SettingsStore::setApPassword(const String &password) {
  if (!validApPassword(password)) {
    return false;
  }
  bool ok = writeString("ap_pass", password);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  settings_.apPassword = password;
  xSemaphoreGive(mutex_);
  return ok;
}

bool SettingsStore::setField(const String &key, const String &value) {
  for (const FieldSpec &f : FIELDS) {
    if (key != f.consoleKey) {
      continue;
    }
    if (key == "ap_pass" && !validApPassword(value)) {
      return false;
    }
    if (!writeString(f.nvsKey, value) && value.length() > 0) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    settings_.*(f.member) = value;
    xSemaphoreGive(mutex_);
    return true;
  }
  return false;
}

void SettingsStore::factoryReset() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  prefs_.clear();
  settings_ = HubSettings();
  xSemaphoreGive(mutex_);
  seedEmptyFieldsFromSecrets();
}

void SettingsStore::printMasked(Print &out) const {
  HubSettings s = snapshot();
  for (const FieldSpec &f : FIELDS) {
    const String &value = s.*(f.member);
    out.printf("  %-16s %s\n", f.consoleKey, f.secret ? mask(value).c_str() : (value.length() ? value.c_str() : "(empty)"));
  }
}
