#include "display_oled.h"
#include "config.h"
#include <U8g2lib.h>
#include <Wire.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C
  u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

static OledScreen curScreen = SCR_IAT;

// ── Nomi e unità schermate ────────────────────────────────────
static const char* SCR_NAME[] = { "IAT", "BOOST", "TRANS", "RPM" };
static const char* SCR_UNIT[] = { "\xb0""C", "bar", "\xb0""C", "rpm" };

// ── Font ─────────────────────────────────────────────────────
#define FONT_VALUE u8g2_font_logisoso32_tn
#define FONT_DEC   u8g2_font_logisoso22_tn
#define FONT_UNIT  u8g2_font_helvR08_tr
#define FONT_BOOT  u8g2_font_logisoso28_tr

// ── Disegna header zona gialla ────────────────────────────────
static void drawHeader(const char* label, const char* unit = nullptr) {
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, label);
  if (unit && unit[0]) {
    int uw = u8g2.getStrWidth(unit);
    u8g2.drawStr(72 - uw / 2, OLED_YEL_BASE, unit);
  }
  // Pallini navigazione
  for (int i = 0; i < SCR_COUNT; i++) {
    int px = 100 + i * 7;
    if (i == (int)curScreen) u8g2.drawDisc(px, OLED_DOT_Y, 2);
    else                     u8g2.drawCircle(px, OLED_DOT_Y, 2);
  }
}

// ── Numero grande + decimale ──────────────────────────────────
static void drawBigValue(float val, int dec) {
  char iS[8], dS[6];
  long iv = (long)val;
  snprintf(iS, sizeof(iS), "%ld", iv);
  if (dec == 1) { long d = labs((long)(val * 10) % 10);   snprintf(dS, sizeof(dS), ".%01ld", d); }
  else          { long d = labs((long)(val * 100) % 100); snprintf(dS, sizeof(dS), ".%02ld", d); }

  u8g2.setFont(FONT_VALUE);
  int wI = u8g2.getStrWidth(iS);
  u8g2.setFont(FONT_DEC);
  int wD = u8g2.getStrWidth(dS);
  int x  = (128 - wI - wD) / 2;

  u8g2.setFont(FONT_VALUE);
  u8g2.drawStr(x, OLED_VALUE_Y, iS);
  u8g2.setFont(FONT_DEC);
  u8g2.drawStr(x + wI, OLED_VALUE_Y - 4, dS);
}

// ── Gauge bar + sub-label ─────────────────────────────────────
static void drawGauge(float val, float mn, float mx,
                      const char* sub, AlertLevel alert) {
  // Barra
  u8g2.drawFrame(OLED_GAUGE_X, OLED_GAUGE_Y, OLED_GAUGE_W, OLED_GAUGE_H);
  float pct = (val - mn) / (mx - mn);
  if (pct < 0) pct = 0; if (pct > 1) pct = 1;
  int fill = (int)((OLED_GAUGE_W - 2) * pct);
  if (fill > 0) u8g2.drawBox(OLED_GAUGE_X + 1, OLED_GAUGE_Y + 1, fill, OLED_GAUGE_H - 2);

  // Sub-label
  if (sub && sub[0]) {
    // Flash testo per danger
    bool show = true;
    if (alert == ALERT_DANGER) show = (millis() / 400) % 2 == 0;
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
}

void DisplayOled::showBoot() {
  // Fase 1 — logo
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CARMETRIX");
  u8g2.setFont(FONT_BOOT);
  int bw = u8g2.getStrWidth("OBD2");
  u8g2.drawStr((128 - bw) / 2, 44, "OBD2");
  u8g2.sendBuffer();
  delay(600);

  // Fase 2 — barra carico
  const int BX=8, BY=50, BW=112, BH=6, STEPS=20;
  for (int i = 0; i <= STEPS; i++) {
    u8g2.clearBuffer();
    u8g2.setFont(FONT_UNIT);
    u8g2.drawStr(2, OLED_YEL_BASE, "CARMETRIX");
    u8g2.setFont(FONT_BOOT);
    u8g2.drawStr((128 - bw) / 2, 44, "OBD2");
    u8g2.drawFrame(BX, BY, BW, BH);
    int f = (BW - 2) * i / STEPS;
    if (f > 0) u8g2.drawBox(BX + 1, BY + 1, f, BH - 2);
    u8g2.sendBuffer();
    delay(i < 10 ? 50 : 20);
  }

  // Fase 3 — doppio flash
  for (int f = 0; f < 2; f++) {
    u8g2.clearBuffer(); u8g2.drawBox(0, 12, 128, 52);
    u8g2.sendBuffer(); delay(60);
    u8g2.clearBuffer(); u8g2.sendBuffer(); delay(60);
  }

  // Fase 4 — READY
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CARMETRIX");
  u8g2.setFont(FONT_BOOT);
  int lw = u8g2.getStrWidth("READY");
  u8g2.drawStr((128 - lw) / 2, 44, "READY");
  u8g2.sendBuffer();
  delay(400);
}

void DisplayOled::showConfigMode(const char* ssid, const char* ip) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CONFIG MODE");
  u8g2.drawStr(0, 24, "WiFi:");
  u8g2.drawStr(0, 35, ssid);
  u8g2.drawStr(0, 49, "carmetrix.local");
  u8g2.drawStr(0, 62, ip);
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

  u8g2.drawStr(0, 28, deviceName);
  // Promemoria comandi pulsante
  u8g2.drawStr(0, 46, "3s = CONFIG");
  u8g2.drawStr(0, 60, "6s = RESET");
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
  u8g2.drawBox(0, 12, 128, 52);  // zona blu piena (bianca sul display)
  u8g2.sendBuffer();
}

void DisplayOled::showMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_UNIT);
  u8g2.drawStr(2, OLED_YEL_BASE, "CARMETRIX");
  if (line1) { int w = u8g2.getStrWidth(line1); u8g2.drawStr((128-w)/2, 32, line1); }
  if (line2) { int w = u8g2.getStrWidth(line2); u8g2.drawStr((128-w)/2, 46, line2); }
  u8g2.sendBuffer();
}

// ── showData — rendering principale ──────────────────────────
void DisplayOled::showData(const OBDData& data, OledScreen scr, AlertLevel alert) {
  u8g2.clearBuffer();

  // Flash zone blu se danger
  if (alert == ALERT_DANGER && (millis() / 300) % 2 == 0) {
    drawHeader(SCR_NAME[scr], SCR_UNIT[scr]);
    u8g2.drawBox(0, 12, 128, 52);
    u8g2.sendBuffer();
    return;
  }

  drawHeader(SCR_NAME[scr], SCR_UNIT[scr]);

  // Seleziona valore corrente
  float val = 0; bool valid = false;
  float mn = 0, mx = 100;
  char sub[24] = "";
  int dec = 1;

  switch (scr) {
    case SCR_IAT:
      val = data.iat.value; valid = data.iat.valid;
      mn = -20; mx = 80;
      strcpy(sub, val > 60 ? "ARIA CALDA" : "NOMINALE"); break;

    case SCR_BOOST:
      val = data.boost.value; valid = data.boost.valid;
      mn = 0; mx = 2.5; dec = 2;
      snprintf(sub, sizeof(sub), "%.1f PSI", val * 14.5038f); break;

    case SCR_TRANS:
      val = data.transTemp.value; valid = data.transTemp.valid;
      mn = 40; mx = 150;
      if (val > 110) strcpy(sub, "!! ALTA TEMP !!");
      else if (val > 90) strcpy(sub, "ATTENZIONE");
      else strcpy(sub, "NOMINALE"); break;

    case SCR_RPM:
      val = data.rpm.value; valid = data.rpm.valid;
      mn = 0; mx = 8000; dec = 0;
      snprintf(sub, sizeof(sub), "%.0f rpm", val); break;

    default: break;
  }

  if (!valid) {
    u8g2.setFont(FONT_UNIT);
    if (!BleElm327::isConnected()) {
      const char* msg = "NO CONNECTION";
      u8g2.drawStr((128 - u8g2.getStrWidth(msg)) / 2, 40, msg);
    } else {
      // Diagnostica: stato init + se la centralina risponde, comando e risposta
      String st = String(BleElm327::initOk() ? "INIT OK" : "INIT FAIL");
      st += BleElm327::ecuOk() ? "  ECU:OK" : "  ECU:--";
      u8g2.drawStr(2, 26, st.c_str());
      String tx = "TX:" + BleElm327::lastCmd();
      u8g2.drawStr(2, 40, tx.c_str());
      String rx = BleElm327::lastRaw();
      rx.replace("\r", " "); rx.replace("\n", " ");
      if (rx.length() == 0) rx = "(nessuna risposta)";
      if (rx.length() > 19)  rx = rx.substring(0, 19);
      u8g2.drawStr(2, 54, ("RX:" + rx).c_str());
    }
  } else {
    drawBigValue(val, dec);
    drawGauge(val, mn, mx, sub, alert);
  }

  u8g2.sendBuffer();
}

void DisplayOled::nextScreen() {
  curScreen = (OledScreen)((curScreen + 1) % SCR_COUNT);
}

OledScreen DisplayOled::currentScreen() { return curScreen; }
