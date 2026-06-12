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

// ── Configurazione buzzer (default = risonanza misurata, max volume) ──
static BuzzerConfig bz = { true, 1700, 2100, "continuo" };
static volatile int testFreq = 0;
static char         testStyle[12] = "";

// ── Default soglie ────────────────────────────────────────────
static const AlertThreshold DEFAULTS[] = {
  { "IAT",     50.0f,  65.0f,  true },
  { "BOOST",    1.8f,   2.2f,  true },
  { "TRANS",   90.0f, 110.0f,  true },
  { "COOLANT", 100.0f, 110.0f, true },
  { "OIL",     120.0f, 140.0f, true },
  { "HVTEMP",  45.0f,  55.0f,  true },
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
  bz.warnFreq   = d["warnFreq"]   | 1700;
  bz.dangerFreq = d["dangerFreq"] | 2100;
  strlcpy(bz.dangerStyle, d["dangerStyle"] | "continuo", sizeof(bz.dangerStyle));
  // Migrazione stili vecchi (rimossi perché sgradevoli)
  if (strcmp(bz.dangerStyle, "sirena") == 0) strlcpy(bz.dangerStyle, "bitonale", sizeof(bz.dangerStyle));
  if (strcmp(bz.dangerStyle, "beep")   == 0) strlcpy(bz.dangerStyle, "impulsi",  sizeof(bz.dangerStyle));
}

const BuzzerConfig& AlertManager::buzzerCfg() { return bz; }

void AlertManager::requestTest(int freq, const char* style) {
  strlcpy(testStyle, style ? style : "", sizeof(testStyle));
  testFreq = freq;
}

// Riproduce il pattern danger nello stile dato. Pattern pensati per un
// buzzer passivo: niente sweep PWM (suona "sveglia"), solo toni netti.
static void playDangerStyle(int f, const char* style) {
  String s = style;
  if (s == "impulsi") {            // beep ritmati netti
    for (int i = 0; i < 4; i++) { beeperTone(f, 140); delay(80); }
  } else if (s == "bitonale") {    // due toni alternati, stile sirena EU
    for (int i = 0; i < 3; i++) { beeperTone(f, 180); beeperTone(f * 4 / 5, 180); }
  } else if (s == "allarme") {     // alternanza rapida, "red alert"
    for (int i = 0; i < 5; i++) { beeperTone(f, 80); beeperTone(f * 17 / 20, 80); }
  } else {                         // "continuo" — tono fisso, max volume
    beeperTone(f, 900);
  }
}

void AlertManager::serviceTest() {
  if (testFreq <= 0) return;
  int f = testFreq;
  testFreq = 0;
  if (testStyle[0]) playDangerStyle(f, testStyle);  // preview stile danger
  else              beeperTone(f, 700);             // tono fisso per tarare la freq.
}

static void playDanger() { playDangerStyle(bz.dangerFreq, bz.dangerStyle); }

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
  check("OIL",     data.oilTemp);
  check("HVTEMP",  data.hvTemp);

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
void AlertManager::beepBoot() {
  // Avvio: due tick + tono pieno, tutto alla risonanza (BUZZER_FREQ_BOOT)
  beeperTone(BUZZER_FREQ_BOOT, 30); delay(35);
  beeperTone(BUZZER_FREQ_BOOT, 30); delay(35);
  beeperTone(BUZZER_FREQ_BOOT, 200);
}

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
