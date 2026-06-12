#include "display_oled.h"
#include "config.h"
#include "ble_elm327.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <math.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C
  u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ── Registry schermate ────────────────────────────────────────
// Tutti i valori standard presenti in OBDData. warn/danger = soglie di
// default per la sub-label (NAN = mai); le soglie ufficiali degli allarmi
// restano in AlertManager (/alerts.json).
struct ScreenDef {
  const char* key;
  const char* name;
  const char* unit;
  float       mn, mx;
  uint8_t     dec;
  float       warn, danger;
  const char* dangerText;
  const OBDValue* (*pick)(const OBDData&);
};

static const ScreenDef SCREENS[] = {
  {"rpm",     "RPM",      "rpm",   0,   8000, 0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.rpm; }},
  {"boost",   "BOOST",    "bar",   0,   2.5f, 2, 1.8f, 2.2f, "!! PERICOLO !!",  [](const OBDData& d){ return &d.boost; }},
  {"map",     "MAP",      "kPa",   0,   250,  0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.map; }},
  {"iat",     "IAT",      "\xb0""C", -20, 80, 1, 50,   65,   "!! ARIA CALDA !!",[](const OBDData& d){ return &d.iat; }},
  {"coolant", "COOLANT",  "\xb0""C", 40, 130, 1, 100,  110,  "!! ALTA TEMP !!", [](const OBDData& d){ return &d.coolant; }},
  {"trans",   "TRANS",    "\xb0""C", 40, 150, 1, 90,   110,  "!! ALTA TEMP !!", [](const OBDData& d){ return &d.transTemp; }},
  {"oil",     "OIL",      "\xb0""C", 40, 160, 1, 120,  140,  "!! ALTA TEMP !!", [](const OBDData& d){ return &d.oilTemp; }},
  {"soc",     "BATT SOC", "%",     0,   100,  0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.hvSoc; }},
  {"hvtemp",  "HV TEMP",  "\xb0""C", 0,  60,  1, 45,   55,   "!! ALTA TEMP !!", [](const OBDData& d){ return &d.hvTemp; }},
  {"speed",   "SPEED",    "km/h",  0,   220,  0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.speed; }},
  {"throttle","THROTTLE", "%",     0,   100,  0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.throttle; }},
  {"load",    "LOAD",     "%",     0,   100,  0, NAN,  NAN,  "",                [](const OBDData& d){ return &d.load; }},
};
static const int SCREEN_DEFS = sizeof(SCREENS) / sizeof(SCREENS[0]);

// Lista attiva: indici nel registry, ordine = ordine di rotazione
static std::vector<uint8_t> activeList;
static int curIdx = 0;   // posizione corrente nella lista attiva

static int defIndexByKey(const char* key) {
  for (int i = 0; i < SCREEN_DEFS; i++)
    if (strcmp(SCREENS[i].key, key) == 0) return i;
  return -1;
}

static void defaultScreens() {
  activeList.clear();
  for (const char* k : { "rpm", "boost", "trans", "iat" })
    activeList.push_back((uint8_t)defIndexByKey(k));
}

void DisplayOled::loadScreens() {
  activeList.clear();
  File f = LittleFS.open("/screens.json", "r");
  if (f) {
    JsonDocument doc;
    if (!deserializeJson(doc, f) && doc.is<JsonArray>()) {
      for (JsonVariant v : doc.as<JsonArray>()) {
        int idx = defIndexByKey(v.as<const char*>() ? v.as<const char*>() : "");
        if (idx >= 0) activeList.push_back((uint8_t)idx);
      }
    }
    f.close();
  }
  if (activeList.empty()) defaultScreens();
  if (curIdx >= (int)activeList.size()) curIdx = 0;
  Serial.printf("[OLED] Schermate attive: %d\n", (int)activeList.size());
}

int DisplayOled::availableCount() { return SCREEN_DEFS; }
DisplayOled::ScreenInfo DisplayOled::availableScreen(int i) {
  const ScreenDef& s = SCREENS[constrain(i, 0, SCREEN_DEFS - 1)];
  return { s.key, s.name, s.unit };
}
int DisplayOled::activeCount() { return (int)activeList.size(); }
const char* DisplayOled::activeKey(int i) {
  if (i < 0 || i >= (int)activeList.size()) return "";
  return SCREENS[activeList[i]].key;
}

// ── Font ─────────────────────────────────────────────────────
#define FONT_VALUE u8g2_font_logisoso32_tn
#define FONT_DEC   u8g2_font_logisoso22_tn
#define FONT_UNIT  u8g2_font_helvR08_tr
#define FONT_BOOT  u8g2_font_logisoso28_tr

// ── Header zona gialla: label, unità, pallini navigazione ────
// Max 5 slot a destra. Se ci sono schermate fuori vista, lo slot di
// bordo diventa una freccetta: ▸ nell'ultimo slot (altre a destra),
// ◂ nel primo (altre a sinistra). Con n ≤ 5 solo pallini.
static void drawHeader(const char* label, const char* unit = nullptr) {
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, label);
  if (unit && unit[0]) {
    int lw = u8g2.getStrWidth(label);
    u8g2.drawStr(2 + lw + 6, OLED_YEL_BASE, unit);
  }

  int n = (int)activeList.size();
  if (n <= 0) return;
  const int SLOTS = 5;
  int win = n < SLOTS ? n : SLOTS;
  int x0  = 126 - win * 7 + 3;   // centro del primo slot

  auto dot = [&](int slot, bool cur) {
    int px = x0 + slot * 7;
    if (cur) u8g2.drawDisc(px, OLED_DOT_Y, 2);
    else     u8g2.drawCircle(px, OLED_DOT_Y, 2);
  };
  auto arrow = [&](int slot, bool right) {
    int px = x0 + slot * 7;
    if (right) u8g2.drawTriangle(px - 2, OLED_DOT_Y - 3, px - 2, OLED_DOT_Y + 3, px + 2, OLED_DOT_Y);
    else       u8g2.drawTriangle(px + 2, OLED_DOT_Y - 3, px + 2, OLED_DOT_Y + 3, px - 2, OLED_DOT_Y);
  };

  if (n <= SLOTS) {              // tutte visibili: solo pallini
    for (int i = 0; i < n; i++) dot(i, i == curIdx);
    return;
  }

  int start, count, slot = 0;
  if (curIdx < SLOTS - 1) {                 // prime 4: pallini + ▸
    start = 0; count = SLOTS - 1;
  } else if (curIdx >= n - (SLOTS - 1)) {   // ultime 4: ◂ + pallini
    start = n - (SLOTS - 1); count = SLOTS - 1;
    arrow(slot++, false);
  } else {                                  // centro: ◂ + 3 pallini + ▸
    start = curIdx - 1; count = SLOTS - 2;
    arrow(slot++, false);
  }
  for (int i = 0; i < count; i++, slot++) dot(slot, start + i == curIdx);
  if (start + count < n) arrow(SLOTS - 1, true);
}

// ── Numero grande + decimale ──────────────────────────────────
static void drawBigValue(float val, int dec) {
  char iS[8], dS[6] = "";
  long iv = (long)val;
  snprintf(iS, sizeof(iS), "%ld", iv);
  if (dec == 1)      { long d = labs((long)(val * 10) % 10);   snprintf(dS, sizeof(dS), ".%01ld", d); }
  else if (dec >= 2) { long d = labs((long)(val * 100) % 100); snprintf(dS, sizeof(dS), ".%02ld", d); }
  // dec == 0 → solo intero, niente ".00" (es. RPM)

  u8g2.setFont(FONT_VALUE);
  int wI = u8g2.getStrWidth(iS);
  int wD = 0;
  if (dS[0]) {
    u8g2.setFont(FONT_DEC);
    wD = u8g2.getStrWidth(dS);
  }
  int x = (128 - wI - wD) / 2;

  u8g2.setFont(FONT_VALUE);
  u8g2.drawStr(x, OLED_VALUE_Y, iS);
  if (dS[0]) {
    u8g2.setFont(FONT_DEC);
    u8g2.drawStr(x + wI, OLED_VALUE_Y - 4, dS);
  }
}

// ── Gauge bar + sub-label (solo warn/danger) ──────────────────
static void drawGauge(float val, float mn, float mx,
                      const char* sub, AlertLevel alert) {
  u8g2.drawFrame(OLED_GAUGE_X, OLED_GAUGE_Y, OLED_GAUGE_W, OLED_GAUGE_H);
  float pct = (val - mn) / (mx - mn);
  if (pct < 0) pct = 0; if (pct > 1) pct = 1;
  int fill = (int)((OLED_GAUGE_W - 2) * pct);
  if (fill > 0) u8g2.drawBox(OLED_GAUGE_X + 1, OLED_GAUGE_Y + 1, fill, OLED_GAUGE_H - 2);

  if (sub && sub[0]) {
    bool show = true;
    if (alert == ALERT_DANGER) show = (millis() / 400) % 2 == 0;  // flash testo
    if (show) {
      u8g2.setFont(FONT_UNIT);
      int w = u8g2.getStrWidth(sub);
      u8g2.drawStr(64 - w / 2, OLED_SUB_Y, sub);
    }
  }
}

// ============================================================
void DisplayOled::begin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  loadScreens();   // LittleFS è già montato dal setup()
}

// ── Boot "cyberpunk": logo che si aggancia tra i glitch ──────
static void bootLogo(const char* big, int dx) {
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2 + dx, OLED_YEL_BASE, "CARMETRIX");
  u8g2.setFont(FONT_BOOT);
  int w = u8g2.getStrWidth(big);
  u8g2.drawStr((128 - w) / 2 + dx, 44, big);
}

// Un frame "glitchato": bande orizzontali del logo slittate a caso,
// blocchi in XOR e righe di rumore. Ogni chiamata è diversa.
static void bootGlitchFrame(const char* big) {
  u8g2.clearBuffer();
  int y = 0;
  while (y < 64) {
    int h  = 6 + random(12);                        // banda 6-17 px
    int dx = random(3) == 0 ? random(13) - 6 : 0;   // 1 banda su 3 slitta ±6
    u8g2.setClipWindow(0, y, 128, min(64, y + h));
    bootLogo(big, dx);
    y += h;
  }
  u8g2.setMaxClipWindow();
  u8g2.setDrawColor(2);                             // XOR: blocchi invertiti
  for (int i = 0, k = 1 + random(2); i < k; i++)
    u8g2.drawBox(random(98), 14 + random(40), 12 + random(20), 2 + random(5));
  for (int i = 0, k = 2 + random(3); i < k; i++)    // righe di rumore
    u8g2.drawHLine(random(70), 12 + random(50), 10 + random(45));
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

void DisplayOled::showBoot() {
  // Fase 1 — il logo "si aggancia" tra i glitch
  for (int i = 0; i < 3; i++) {
    bootGlitchFrame("OBD2");
    delay(50 + random(40));
    u8g2.clearBuffer(); bootLogo("OBD2", 0); u8g2.sendBuffer();
    delay(90 + random(80));
  }
  delay(250);

  // Fase 2 — barra carico con disturbi sporadici
  const int BX=8, BY=50, BW=112, BH=6, STEPS=20;
  for (int i = 0; i <= STEPS; i++) {
    u8g2.clearBuffer();
    bootLogo("OBD2", random(8) == 0 ? random(5) - 2 : 0);
    u8g2.drawFrame(BX, BY, BW, BH);
    int f = (BW - 2) * i / STEPS;
    if (f > 0) u8g2.drawBox(BX + 1, BY + 1, f, BH - 2);
    if (random(5) == 0) {            // blocco XOR di disturbo
      u8g2.setDrawColor(2);
      u8g2.drawBox(random(90), 18 + random(36), 14 + random(24), 3 + random(5));
      u8g2.setDrawColor(1);
    }
    u8g2.sendBuffer();
    delay(i < 10 ? 50 : 20);
  }

  // Fase 3 — burst di glitch + flash al posto del doppio flash
  for (int i = 0; i < 4; i++) { bootGlitchFrame("OBD2"); delay(45); }
  u8g2.clearBuffer(); u8g2.drawBox(0, 17, 128, 47); u8g2.sendBuffer(); delay(50);
  u8g2.clearBuffer(); u8g2.sendBuffer(); delay(60);

  // Fase 4 — READY: un glitch d'assestamento, poi stabile + suono boot
  bootGlitchFrame("READY");
  delay(60);
  u8g2.clearBuffer(); bootLogo("READY", 0); u8g2.sendBuffer();
  AlertManager::beepBoot();
  delay(300);
}

void DisplayOled::showConfigMode(const char* ssid, const char* ip) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CONFIG MODE");
  u8g2.drawStr(0, 28, "WiFi:");
  u8g2.drawStr(0, 39, ssid);
  u8g2.drawStr(0, 52, "carmetrix.local");
  u8g2.drawStr(0, 63, ip);
  u8g2.sendBuffer();
}

void DisplayOled::showConnecting(const char* deviceName) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CONNECTING");
  // Animazione pallini nella gialla
  int dots = (millis() / 400) % 4;
  char buf[5] = "";
  for (int i = 0; i < dots; i++) strcat(buf, ".");
  u8g2.drawStr(86, OLED_YEL_BASE, buf);

  u8g2.drawStr(0, 30, deviceName);
  // Promemoria comandi pulsante
  u8g2.drawStr(0, 48, "3s = CONFIG");
  u8g2.drawStr(0, 62, "6s = RESET");
  u8g2.sendBuffer();
}

void DisplayOled::showWaiting() {
  u8g2.clearBuffer();
  drawHeader("OBD2", nullptr);
  u8g2.setFont(FONT_UNIT);
  int w = u8g2.getStrWidth("WAITING...");
  u8g2.drawStr((128 - w) / 2, 40, "WAITING...");
  u8g2.sendBuffer();
}

void DisplayOled::showNoConnection() {
  u8g2.clearBuffer();
  drawHeader("OBD2", nullptr);
  u8g2.setFont(FONT_UNIT);
  int w = u8g2.getStrWidth("NO CONNECTION");
  u8g2.drawStr((128 - w) / 2, 38, "NO CONNECTION");
  u8g2.drawStr(2, 52, "3s = CONFIG");
  u8g2.sendBuffer();
}

void DisplayOled::showFlash() {
  u8g2.clearBuffer();
  u8g2.drawBox(0, 17, 128, 47);  // zona blu piena (bianca sul display)
  u8g2.sendBuffer();
}

void DisplayOled::showMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CARMETRIX");
  if (line1) { int w = u8g2.getStrWidth(line1); u8g2.drawStr((128-w)/2, 34, line1); }
  if (line2) { int w = u8g2.getStrWidth(line2); u8g2.drawStr((128-w)/2, 48, line2); }
  u8g2.sendBuffer();
}

// ── Conferma factory reset ────────────────────────────────────
// heldMs = pressione in corso (0 se rilasciato): barra di avanzamento
// verso i 6s di conferma.
void DisplayOled::showResetConfirm(unsigned long heldMs) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "FACTORY RESET");

  const char* q = "CONFERMA RESET?";
  int w = u8g2.getStrWidth(q);
  u8g2.drawStr((128 - w) / 2, 32, q);
  const char* hint = "6s = RESET   3s = NO";
  w = u8g2.getStrWidth(hint);
  u8g2.drawStr((128 - w) / 2, 46, hint);

  // Avanzamento pressione verso i 6s
  u8g2.drawFrame(OLED_GAUGE_X, 54, OLED_GAUGE_W, 6);
  if (heldMs > 0) {
    float pct = (float)heldMs / BTN_FACTORY_RESET;
    if (pct > 1) pct = 1;
    int fill = (int)((OLED_GAUGE_W - 2) * pct);
    if (fill > 0) u8g2.drawBox(OLED_GAUGE_X + 1, 55, fill, 4);
  }
  u8g2.sendBuffer();
}

// ── showData — rendering principale ──────────────────────────
void DisplayOled::showData(const OBDData& data, int scrIdx, AlertLevel alert) {
  if (activeList.empty()) { showWaiting(); return; }
  if (scrIdx < 0 || scrIdx >= (int)activeList.size()) scrIdx = 0;
  const ScreenDef& S = SCREENS[activeList[scrIdx]];
  const OBDValue* v  = S.pick(data);

  u8g2.clearBuffer();

  // Flash zona blu se danger
  if (alert == ALERT_DANGER && (millis() / 300) % 2 == 0) {
    drawHeader(S.name, S.unit);
    u8g2.drawBox(0, 17, 128, 47);
    u8g2.sendBuffer();
    return;
  }

  drawHeader(S.name, S.unit);

  if (!v->valid) {
    u8g2.setFont(FONT_UNIT);
    if (!BleElm327::isConnected()) {
      const char* msg = "NO CONNECTION";
      u8g2.drawStr((128 - u8g2.getStrWidth(msg)) / 2, 40, msg);
    } else {
      // Diagnostica: stato init + se la centralina risponde, comando e risposta
      String st = String(BleElm327::initOk() ? "INIT OK" : "INIT FAIL");
      st += BleElm327::ecuOk() ? "  ECU:OK" : "  ECU:--";
      u8g2.drawStr(2, 28, st.c_str());
      String tx = "TX:" + BleElm327::lastCmd();
      u8g2.drawStr(2, 42, tx.c_str());
      String rx = BleElm327::lastRaw();
      rx.replace("\r", " "); rx.replace("\n", " ");
      if (rx.length() == 0) rx = "(nessuna risposta)";
      if (rx.length() > 19)  rx = rx.substring(0, 19);
      u8g2.drawStr(2, 56, ("RX:" + rx).c_str());
    }
  } else {
    drawBigValue(v->value, S.dec);
    // Sub-label solo in warn/danger (in nominale la riga resta vuota)
    const char* sub = "";
    if (!isnan(S.danger) && v->value >= S.danger)    sub = S.dangerText;
    else if (!isnan(S.warn) && v->value >= S.warn)   sub = "ATTENZIONE";
    drawGauge(v->value, S.mn, S.mx, sub, alert);
  }

  u8g2.sendBuffer();
}

void DisplayOled::nextScreen() {
  if (activeList.empty()) return;
  curIdx = (curIdx + 1) % (int)activeList.size();
}

void DisplayOled::setScreen(int idx) {
  if (idx >= 0 && idx < (int)activeList.size()) curIdx = idx;
}

int DisplayOled::currentScreen() { return curIdx; }
