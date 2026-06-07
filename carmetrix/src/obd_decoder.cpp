#include "obd_decoder.h"
#include "ble_elm327.h"
#include "config.h"
#include "profile_loader.h"
#include <math.h>

// ── Helper formula semplice A*k+q ────────────────────────────
// Supporta: "A*k+q", "A*k-q", "(A*256+B)/k", "(A-40)"
static float evalFormula(const char* formula, uint8_t A, uint8_t B) {
  // Parser minimo per le formule più comuni nei profili JSON
  String f = formula;
  f.replace(" ", "");

  // Sostituisce A e B con i valori numerici
  char bufA[8], bufB[8];
  snprintf(bufA, sizeof(bufA), "%d", A);
  snprintf(bufB, sizeof(bufB), "%d", B);

  // Casi comuni hardcoded per non portare un parser espressioni completo
  // "A-40"
  if (f == "A-40") return A - 40.0f;
  // "A*100/255"
  if (f == "A*100/255") return A * 100.0f / 255.0f;
  // "(256*A+B)/4"
  if (f == "(256*A+B)/4" || f == "(A*256+B)/4")
    return ((256.0f * A + B) / 4.0f);
  // "A"
  if (f == "A") return (float)A;
  // "A/2-40"
  if (f == "A/2-40") return A / 2.0f - 40.0f;
  // "(A*256+B)*0.1"
  if (f == "(A*256+B)*0.1") return (A * 256.0f + B) * 0.1f;

  // Fallback generico: interpreta "A*k" o "A*k+q" o "A*k-q"
  // Cerca moltiplicatore dopo 'A*'
  int starPos = f.indexOf("A*");
  if (starPos >= 0) {
    float k = f.substring(starPos + 2).toFloat();
    float base = A * k;
    int plusPos  = f.indexOf('+', starPos + 2);
    int minusPos = f.indexOf('-', starPos + 2);
    if (plusPos  > 0) base += f.substring(plusPos + 1).toFloat();
    if (minusPos > 0) base -= f.substring(minusPos + 1).toFloat();
    return base;
  }
  return (float)A;
}

// ============================================================
//  DECODIFICA MODE 01 — Standard OBD2
// ============================================================
OBDValue OBDDecoder::decodeMode01(uint8_t pid, const ElmResponse& r) {
  OBDValue v;
  v.valid = false;
  v.value = 0;

  if (!r.ok || r.len < 1) return v;

  uint8_t A = r.bytes[0];
  uint8_t B = r.len > 1 ? r.bytes[1] : 0;

  v.valid = true;

  switch (pid) {
    case 0x04: // Engine Load
      v.value = A * 100.0f / 255.0f;
      strlcpy(v.name, "LOAD",     sizeof(v.name));
      strlcpy(v.unit, "%",        sizeof(v.unit)); break;

    case 0x05: // Coolant Temp
      v.value = A - 40.0f;
      strlcpy(v.name, "COOLANT",  sizeof(v.name));
      strlcpy(v.unit, "\xb0""C",  sizeof(v.unit)); break;

    case 0x0B: // MAP kPa
      v.value = (float)A;
      strlcpy(v.name, "MAP",      sizeof(v.name));
      strlcpy(v.unit, "kPa",      sizeof(v.unit)); break;

    case 0x0C: // RPM
      v.value = (A * 256.0f + B) / 4.0f;
      strlcpy(v.name, "RPM",      sizeof(v.name));
      strlcpy(v.unit, "rpm",      sizeof(v.unit)); break;

    case 0x0D: // Speed
      v.value = (float)A;
      strlcpy(v.name, "SPEED",    sizeof(v.name));
      strlcpy(v.unit, "km/h",     sizeof(v.unit)); break;

    case 0x0F: // IAT
      v.value = A - 40.0f;
      strlcpy(v.name, "IAT",      sizeof(v.name));
      strlcpy(v.unit, "\xb0""C",  sizeof(v.unit)); break;

    case 0x11: // Throttle
      v.value = A * 100.0f / 255.0f;
      strlcpy(v.name, "THROTTLE", sizeof(v.name));
      strlcpy(v.unit, "%",        sizeof(v.unit)); break;

    case 0x5F: // Trans Temp (Mode 01, se supportato)
      v.value = A - 40.0f;
      strlcpy(v.name, "TRANS",    sizeof(v.name));
      strlcpy(v.unit, "\xb0""C",  sizeof(v.unit)); break;

    default:
      v.valid = false; break;
  }
  return v;
}

// ============================================================
//  DECODIFICA MODE 22 — Proprietario
// ============================================================
OBDValue OBDDecoder::decodeMode22(const char* name, const char* unit,
                                   const char* formula, const ElmResponse& r) {
  OBDValue v;
  v.valid = false;
  v.value = 0;
  strlcpy(v.name, name, sizeof(v.name));
  strlcpy(v.unit, unit, sizeof(v.unit));

  if (!r.ok || r.len < 1) return v;
  v.value = evalFormula(formula, r.bytes[0], r.len > 1 ? r.bytes[1] : 0);
  v.valid = true;
  return v;
}

// ============================================================
//  POLL COMPLETO
// ============================================================
static unsigned long lastPollMs = 0;

void OBDDecoder::pollAll(OBDData& data, bool hasExtended) {
  if (millis() - lastPollMs < OBD_POLL_INTERVAL) return;
  lastPollMs = millis();

  if (!BleElm327::isConnected()) return;

  // ── Mode 01 standard ─────────────────────────────────────
  auto r0F = BleElm327::queryPID(0x01, 0x0F);
  data.iat = decodeMode01(0x0F, r0F);

  auto r0B = BleElm327::queryPID(0x01, 0x0B);
  data.map = decodeMode01(0x0B, r0B);

  // Boost = MAP - pressione atmosferica (approssimata 101.3 kPa)
  if (data.map.valid) {
    data.boost.value = (data.map.value - 101.3f) / 100.0f; // bar
    data.boost.valid = true;
    strlcpy(data.boost.name, "BOOST",   sizeof(data.boost.name));
    strlcpy(data.boost.unit, "bar",     sizeof(data.boost.unit));
  }

  auto r05 = BleElm327::queryPID(0x01, 0x05);
  data.coolant = decodeMode01(0x05, r05);

  auto r0C = BleElm327::queryPID(0x01, 0x0C);
  data.rpm = decodeMode01(0x0C, r0C);

  auto r0D = BleElm327::queryPID(0x01, 0x0D);
  data.speed = decodeMode01(0x0D, r0D);

  auto r11 = BleElm327::queryPID(0x01, 0x11);
  data.throttle = decodeMode01(0x11, r11);

  // ── Mode 22 esteso (profilo produttore) ──────────────────
  if (hasExtended) {
    const auto& pids = ProfileLoader::getExtendedPIDs();
    for (const auto& p : pids) {
      // Query Mode 22 con parsing byte corretto (header "62"+pid)
      auto re = BleElm327::queryPID22(p.pidHex);
      OBDValue v = decodeMode22(p.name, p.unit, p.formula, re);
      if (!v.valid) continue;
      switch (p.target) {
        case ProfileLoader::TARGET_TRANS_TEMP: data.transTemp = v; break;
        case ProfileLoader::TARGET_BOOST:      data.boost     = v; break;
        // oil_temp / turbo_rpm / custom: estendibili qui
        default: break;
      }
    }
  }

  data.lastUpdateMs = millis();
  data.anyValid = data.iat.valid || data.boost.valid ||
                  data.coolant.valid || data.rpm.valid;
}

// ============================================================
//  DEMO MODE — dati sintetici
// ============================================================
static unsigned long lastDemoMs = 0;
static float demoT = 0;

void OBDDecoder::generateDemo(OBDData& data) {
  if (millis() - lastDemoMs < OBD_POLL_INTERVAL) return;
  lastDemoMs = millis();
  demoT += 0.05f;

  auto setV = [](OBDValue& o, float val, const char* name, const char* unit) {
    o.value = val; o.valid = true;
    strlcpy(o.name, name, sizeof(o.name));
    strlcpy(o.unit, unit, sizeof(o.unit));
  };

  // Onde a frequenze diverse → i valori attraversano warn e danger
  // IAT: 5..65 °C   (warn 50, danger 65)
  setV(data.iat,   35.0f + 30.0f * sinf(demoT * 0.30f),         "IAT",   "\xb0""C");
  // BOOST: 0.3..2.3 bar (warn 1.8, danger 2.2)
  setV(data.boost, fmaxf(0.0f, 1.3f + 1.0f * sinf(demoT * 0.55f)), "BOOST", "bar");
  // TRANS: 75..115 °C (warn 90, danger 110)
  setV(data.transTemp, 95.0f + 20.0f * sinf(demoT * 0.18f),    "TRANS", "\xb0""C");
  // COOLANT: 70..110 °C
  setV(data.coolant, 90.0f + 20.0f * sinf(demoT * 0.12f),      "COOLANT","\xb0""C");
  // RPM: 800..6500
  setV(data.rpm, 800.0f + 5700.0f * (0.5f + 0.5f * sinf(demoT * 0.7f)), "RPM", "rpm");
  // SPEED: 0..180
  setV(data.speed, 90.0f + 90.0f * sinf(demoT * 0.25f),        "SPEED", "km/h");
  // MAP coerente col boost
  data.map.value = (data.boost.value * 100.0f) + 101.3f;
  data.map.valid = true;

  data.lastUpdateMs = millis();
  data.anyValid = true;
}

void OBDDecoder::reset(OBDData& data) {
  memset(&data, 0, sizeof(data));
}
