#include "alert_manager.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

static std::vector<AlertThreshold> thresholds;
static AlertLevel  currentAlertLevel = ALERT_NONE;
static bool        flashFlag         = false;
static unsigned long lastBuzzerMs    = 0;
static bool        buzzerActive      = false;

// ── Default soglie ────────────────────────────────────────────
static const AlertThreshold DEFAULTS[] = {
  { "IAT",     50.0f,  65.0f,  true },
  { "BOOST",    1.8f,   2.2f,  true },
  { "TRANS",   90.0f, 110.0f,  true },
  { "COOLANT", 100.0f, 110.0f, true },
};

// ── Buzzer helper (rinominato per evitare conflitto con tone() di Arduino) ──
static void beeperTone(uint32_t freq, uint32_t durationMs) {
  ledcWriteTone(PIN_BUZZER, freq);   // ESP32 core v3: ledcWriteTone(pin, freq)
  delay(durationMs);
  ledcWriteTone(PIN_BUZZER, 0);
}

// ============================================================
void AlertManager::begin() {
  // ESP32 core v3.x: ledcAttach(pin, freq, resolution) — sostituisce
  // ledcSetup + ledcAttachPin della v2
  ledcAttach(PIN_BUZZER, BUZZER_FREQ_WARN, BUZZER_RESOLUTION);
  ledcWriteTone(PIN_BUZZER, 0);

  // Carica soglie (crea defaults se non esistono)
  loadThresholds();
  if (thresholds.empty()) {
    for (auto& d : DEFAULTS) thresholds.push_back(d);
    saveThresholds();
  }
}

void AlertManager::loadThresholds() {
  thresholds.clear();
  if (!LittleFS.exists("/alerts.json")) return;

  File f = LittleFS.open("/alerts.json", "r");
  if (!f) return;

  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  for (JsonObject obj : doc.as<JsonArray>()) {
    AlertThreshold t;
    strlcpy(t.pidName, obj["pid"] | "", sizeof(t.pidName));
    t.warnVal       = obj["warn"]   | 0.0f;
    t.dangerVal     = obj["danger"] | 0.0f;
    t.buzzerEnabled = obj["buzzer"] | true;
    if (t.pidName[0] != '\0') thresholds.push_back(t);
  }
}

void AlertManager::saveThresholds() {
  File f = LittleFS.open("/alerts.json", "w");
  if (!f) return;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& t : thresholds) {
    JsonObject obj = arr.add<JsonObject>();
    obj["pid"]    = t.pidName;
    obj["warn"]   = t.warnVal;
    obj["danger"] = t.dangerVal;
    obj["buzzer"] = t.buzzerEnabled;
  }
  serializeJson(doc, f);
  f.close();
}

// ── Valuta un singolo valore contro le soglie ─────────────────
static AlertLevel checkValue(const char* pidName, float val, bool valid) {
  if (!valid) return ALERT_NONE;
  for (auto& t : thresholds) {
    if (strcmp(t.pidName, pidName) == 0) {
      if (val >= t.dangerVal) return ALERT_DANGER;
      if (val >= t.warnVal)   return ALERT_WARN;
      return ALERT_NONE;
    }
  }
  return ALERT_NONE;
}

AlertLevel AlertManager::evaluate(const OBDData& data) {
  AlertLevel worst = ALERT_NONE;

  auto check = [&](const char* name, const OBDValue& v) {
    AlertLevel l = checkValue(name, v.value, v.valid);
    if (l > worst) worst = l;
  };

  check("IAT",     data.iat);
  check("BOOST",   data.boost);
  check("TRANS",   data.transTemp);
  check("COOLANT", data.coolant);

  // Aggiorna stato globale
  if (worst != currentAlertLevel) {
    currentAlertLevel = worst;
    if (worst == ALERT_DANGER) { flashFlag = true; beepDanger(); }
    else if (worst == ALERT_WARN) { flashFlag = true; beepWarn(); }
    else { beepStop(); flashFlag = false; }
  } else if (worst == ALERT_DANGER) {
    // Ripete beep pericolo ogni 2 secondi
    if (millis() - lastBuzzerMs > 2000) {
      beepDanger();
      flashFlag = true;
    }
  }

  return worst;
}

// ── Feedback sonori ───────────────────────────────────────────
void AlertManager::beepConfirm() {
  beeperTone(1000, 80); delay(50); beeperTone(1400, 80);
  lastBuzzerMs = millis();
}

void AlertManager::beepWarn() {
  beeperTone(BUZZER_FREQ_WARN, 200); delay(100);
  beeperTone(BUZZER_FREQ_WARN, 200);
  lastBuzzerMs = millis();
}

void AlertManager::beepDanger() {
  for (int i = 0; i < 4; i++) {
    beeperTone(BUZZER_FREQ_DANGER, 100); delay(80);
  }
  lastBuzzerMs = millis();
}

void AlertManager::beepError() {
  beeperTone(1200, 150); delay(60);
  beeperTone(900,  150); delay(60);
  beeperTone(600,  250);
  lastBuzzerMs = millis();
}

void AlertManager::beepStop() {
  ledcWriteTone(PIN_BUZZER, 0);
}

bool AlertManager::shouldFlash() {
  bool f = flashFlag;
  flashFlag = false;
  return f;
}

AlertLevel AlertManager::currentLevel() { return currentAlertLevel; }
