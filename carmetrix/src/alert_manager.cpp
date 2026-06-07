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

// ── Configurazione buzzer (default = forte e acuto, non "sveglia") ──
static BuzzerConfig bz = { true, 1800, 2700, "sirena" };
static volatile int testFreq = 0;

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
  loadBuzzerCfg();
}

// ── Configurazione buzzer ─────────────────────────────────────
void AlertManager::loadBuzzerCfg() {
  if (!LittleFS.exists("/buzzer.json")) return;   // mantiene i default
  File f = LittleFS.open("/buzzer.json", "r");
  if (!f) return;
  JsonDocument d;
  if (deserializeJson(d, f)) { f.close(); return; }
  f.close();
  bz.enabled    = d["enabled"]    | true;
  bz.warnFreq   = d["warnFreq"]   | 1800;
  bz.dangerFreq = d["dangerFreq"] | 2700;
  strlcpy(bz.dangerStyle, d["dangerStyle"] | "sirena", sizeof(bz.dangerStyle));
}

const BuzzerConfig& AlertManager::buzzerCfg() { return bz; }

void AlertManager::requestTest(int freq) { testFreq = freq; }

void AlertManager::serviceTest() {
  if (testFreq <= 0) return;
  int f = testFreq;
  testFreq = 0;
  beeperTone(f, 700);   // tono continuo per valutare volume/acutezza
}

// Riproduce il tono danger secondo lo stile configurato
static void playDanger() {
  int f = bz.dangerFreq;
  String s = bz.dangerStyle;
  if (s == "continuo") {
    beeperTone(f, 900);
  } else if (s == "beep") {
    for (int i = 0; i < 3; i++) { beeperTone(f, 110); delay(70); }
  } else {  // "sirena" — sweep su/giù, acuto e penetrante
    for (int sweep = 0; sweep < 2; sweep++) {
      for (int x = f * 6 / 10; x <= f; x += 90) { ledcWriteTone(PIN_BUZZER, x); delay(14); }
      for (int x = f; x >= f * 6 / 10; x -= 90) { ledcWriteTone(PIN_BUZZER, x); delay(14); }
    }
    ledcWriteTone(PIN_BUZZER, 0);
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
  if (!bz.enabled) return;
  beeperTone(bz.warnFreq, 180); delay(90);
  beeperTone(bz.warnFreq, 180);
  lastBuzzerMs = millis();
}

void AlertManager::beepDanger() {
  if (!bz.enabled) return;
  playDanger();
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
