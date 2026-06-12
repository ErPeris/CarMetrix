#include "obd_decoder.h"
#include "ble_elm327.h"
#include "config.h"
#include "profile_loader.h"
#include <math.h>
#include <vector>
#include <algorithm>

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
//  DECODIFICA STRUTTURATA (profili schema 2, formato OBDb)
// ============================================================
// Estrae `len` bit big-endian a partire dal bit `bix` del payload
// (dopo l'echo 62+pid), applica sign/mul/div/add e clampa su mn/mx.
static OBDValue decodeStructured(const ProfileLoader::ExtendedPID& p,
                                 const ElmResponse& r) {
  OBDValue v;
  v.valid = false; v.value = 0;
  strlcpy(v.name, p.name, sizeof(v.name));
  strlcpy(v.unit, p.unit, sizeof(v.unit));

  if (!r.ok) return v;
  uint16_t lastBit = p.bix + p.len;
  if (p.len == 0 || p.len > 32) return v;
  if ((uint16_t)((lastBit + 7) / 8) > r.len) return v;  // payload troppo corto

  uint32_t raw = 0;
  for (uint16_t b = 0; b < p.len; b++) {
    uint16_t bitIdx = p.bix + b;
    uint8_t byte = r.bytes[bitIdx / 8];
    raw = (raw << 1) | ((byte >> (7 - (bitIdx % 8))) & 1);
  }

  float val;
  if (p.sign && p.len < 32 && (raw & (1UL << (p.len - 1)))) {
    val = (float)(int32_t)(raw | (~0UL << p.len));   // estensione di segno
  } else {
    val = (float)raw;
  }
  val = val * p.mul / p.div + p.add;

  if (val < p.mn || val > p.mx) return v;  // fuori range fisico → scarta
  v.value = val;
  v.valid = true;
  return v;
}

// ============================================================
//  POLLING NON BLOCCANTE (stile Car Scanner)
// ============================================================
// Una sola query in volo alla volta: pollTick invia, ritorna subito, e al
// giro di loop successivo controlla se è arrivato il prompt '>'. Appena
// arriva decodifica e invia il PID successivo → nessun busy-wait, display
// sempre fluido. PID lenti (temperature) hanno uno skip-rate; PID che
// falliscono ripetutamente vengono sospesi e ritentati più tardi.

// Maschera 0100 (PID 01-20): i PID che la ECU dichiara di non supportare
// non vengono interrogati (es. Civic in 29-bit funzionale: niente 05/0F).
static uint32_t supportedMask1 = 0;
void OBDDecoder::setSupportedMask(uint32_t mask1) { supportedMask1 = mask1; }
static bool pidSupported(uint8_t pid) {
  if (supportedMask1 == 0) return true;  // maschera ignota → prova tutto
  if (pid < 0x01 || pid > 0x20) return true;
  return supportedMask1 & (1UL << (32 - pid));
}

struct PollItem {
  uint8_t       mode;            // 0x01 o 0x22
  uint16_t      pid;
  uint8_t       every;           // interroga ogni N giri (1 = sempre)
  uint8_t       fails;           // fallimenti consecutivi
  unsigned long suspendedUntil;  // 0 = attivo
  int8_t        extIdx;          // indice PID esteso del profilo, -1 = Mode 01
};

static std::vector<PollItem> pollList;
static uint32_t builtMask     = 0xFFFFFFFF;  // sentinel: lista non costruita
static bool     builtExtended = false;
static size_t   pollIdx  = 0;
static uint32_t roundNum = 0;
static bool     awaiting = false;
static char     awaitingHdr[8];

// Mini-coda comandi AT per lo switch header (profili schema 2: PID su ECU
// diverse). Prima di una query con hdr diverso dall'attivo si inviano
// ATSH/ATCRA, uno per tick, poi parte la query vera.
static String  pendingCmds[6];
static int     pendingCount = 0, pendingIdx = 0;
static bool    pendingAwaiting = false;
static String  pendingTarget;

static const char* itemHdr(const PollItem& it) {
  if (it.extIdx < 0) return "";
  return ProfileLoader::getExtendedPIDs()[it.extIdx].hdr;
}

// true = switch accodato (chiamante deve attendere i prossimi tick).
// session/fc opzionali (profili schema 2): dopo ATSH/ATCRA si accodano
// ATFCSH<fc>+ATFCSM1 (flow control multi-frame) e la query di sessione UDS.
static bool queueHeaderSwitch(const char* want, const char* rax,
                              const char* session = "", const char* fc = "") {
  pendingTarget = want;
  int n = want[0]
    ? BleElm327::getHeaderCmds(want, rax, pendingCmds)
    : BleElm327::getDefaultHeaderCmds(pendingCmds);
  if (n == 0) {  // protocollo senza header gestiti: nulla da fare
    pendingCount = 0;
    BleElm327::setActiveHdr(want);
    return false;
  }
  if (fc && fc[0] && n <= 4) {           // flow control: 2 comandi AT
    pendingCmds[n++] = String("ATFCSH") + fc;
    pendingCmds[n++] = "ATFCSM1";
  }
  if (session && session[0] && n <= 5)   // sessione UDS: 1 query (risposta ignorata)
    pendingCmds[n++] = session;
  pendingCount = n;
  pendingIdx = 0;
  pendingAwaiting = false;
  return true;
}

// Standby: tutti i PID veloci sospesi (quadro spento, ECU muta con BLE su).
// Niente giri normali: un solo probe 0100 ogni OBD_STANDBY_PROBE_MS, alla
// prima risposta si azzerano le sospensioni e riparte il polling pieno.
static bool          standby = false;
static bool          standbyAwaiting = false;
static unsigned long lastStandbyProbeMs = 0;

bool OBDDecoder::isStandby() { return standby; }

static void buildPollList(bool hasExtended) {
  pollList.clear();
  auto add = [](uint8_t mode, uint16_t pid, uint8_t every, int8_t extIdx) {
    pollList.push_back({mode, pid, every, 0, 0, extIdx});
  };
  // Veloci: ogni giro
  if (pidSupported(0x0B)) add(0x01, 0x0B, 1, -1);  // MAP → boost
  if (pidSupported(0x0C)) add(0x01, 0x0C, 1, -1);  // RPM
  if (pidSupported(0x0D)) add(0x01, 0x0D, 1, -1);  // speed
  if (pidSupported(0x11)) add(0x01, 0x11, 1, -1);  // throttle
  // Lente: ogni 4 giri (cambiano in secondi, non in millisecondi)
  if (pidSupported(0x05)) add(0x01, 0x05, 4, -1);  // coolant
  if (pidSupported(0x0F)) add(0x01, 0x0F, 4, -1);  // IAT
  if (hasExtended) {
    // Raggruppa per header ECU: minimizza gli switch ATSH/ATCRA nel giro
    const auto& pids = ProfileLoader::getExtendedPIDs();
    std::vector<int> order;
    for (size_t i = 0; i < pids.size(); i++) order.push_back((int)i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      return strcmp(pids[a].hdr, pids[b].hdr) < 0;
    });
    for (int i : order)
      add(pids[i].mode, pids[i].pidHex,
          pids[i].every ? pids[i].every : 4, (int8_t)i);
  }
  builtMask     = supportedMask1;
  builtExtended = hasExtended;
  pollIdx = 0; roundNum = 0; awaiting = false;
  standby = false; standbyAwaiting = false;
  pendingCount = 0; pendingAwaiting = false;
}

// Quadro spento? Tutti gli item Mode 01 a giro pieno (every==1) sospesi.
// I soli Mode 22 sospesi non bastano (possono essere PID sbagliati).
static bool allFastPidsSuspended() {
  bool anyFast = false;
  for (const auto& it : pollList) {
    if (it.mode != 0x01 || it.every != 1) continue;
    anyFast = true;
    if (it.suspendedUntil == 0) return false;
  }
  return anyFast;
}

static void clearSuspensions() {
  for (auto& it : pollList) { it.suspendedUntil = 0; it.fails = 0; }
}

static void applyMode01(OBDData& d, uint8_t pid, const OBDValue& v) {
  // Un poll fallito NON azzera il valore: si tiene l'ultimo valido
  if (!v.valid) return;
  switch (pid) {
    case 0x05: d.coolant  = v; break;
    case 0x0F: d.iat      = v; break;
    case 0x0C: d.rpm      = v; break;
    case 0x0D: d.speed    = v; break;
    case 0x11: d.throttle = v; break;
    case 0x04: d.load     = v; break;
    case 0x5F: d.transTemp = v; break;
    case 0x0B:
      d.map = v;
      // Boost = MAP - pressione atmosferica (approssimata 101.3 kPa)
      d.boost.value = (v.value - 101.3f) / 100.0f;  // bar
      d.boost.valid = true;
      strlcpy(d.boost.name, "BOOST", sizeof(d.boost.name));
      strlcpy(d.boost.unit, "bar",   sizeof(d.boost.unit));
      break;
    default: break;
  }
}

void OBDDecoder::pollTick(OBDData& data, bool hasExtended) {
  if (!BleElm327::isConnected()) { awaiting = false; return; }
  if (builtMask != supportedMask1 || builtExtended != hasExtended ||
      pollList.empty())
    buildPollList(hasExtended);
  if (pollList.empty()) return;

  // ── Switch header in corso? (un comando AT per tick) ─────
  if (pendingCount > 0) {
    if (pendingAwaiting) {
      String raw;
      int st = BleElm327::pollAsync(raw, OBD_QUERY_TIMEOUT);
      if (st == 0) return;
      pendingAwaiting = false;
      if (st < 0) {           // timeout AT: stato header incerto
        BleElm327::setActiveHdr("?");
        pendingCount = 0;
        return;
      }
      pendingIdx++;
      if (pendingIdx >= pendingCount) {
        BleElm327::setActiveHdr(pendingTarget.c_str());
        pendingCount = 0;
      }
    }
    if (pendingCount > 0 && !pendingAwaiting) {
      if (BleElm327::sendAsync(pendingCmds[pendingIdx].c_str()))
        pendingAwaiting = true;
    }
    return;
  }

  // ── Standby: solo probe 0100 ogni OBD_STANDBY_PROBE_MS ───
  if (standby) {
    if (standbyAwaiting) {
      String raw;
      int st = BleElm327::pollAsync(raw, OBD_QUERY_TIMEOUT);
      if (st == 0) return;
      standbyAwaiting = false;
      if (st == 1 && BleElm327::parseResponse(raw, "4100").ok) {
        Serial.println("[OBD] ECU di nuovo attiva -> riprendo il polling");
        clearSuspensions();
        standby = false;
        awaiting = false;
      }
      return;
    }
    if (millis() - lastStandbyProbeMs >= OBD_STANDBY_PROBE_MS) {
      // Il probe va fatto sull'header di default (ECM/funzionale)
      if (strcmp(BleElm327::activeHdr(), "") != 0) {
        if (queueHeaderSwitch("", "")) return;
      }
      lastStandbyProbeMs = millis();
      char cmd[8];
      int n = BleElm327::getEcuCount();
      if (n >= 1 && n <= 9) snprintf(cmd, sizeof(cmd), "0100%d", n);
      else                  snprintf(cmd, sizeof(cmd), "0100");
      if (BleElm327::sendAsync(cmd)) standbyAwaiting = true;
    }
    return;
  }

  // ── Risposta in volo? ────────────────────────────────────
  if (awaiting) {
    String raw;
    int st = BleElm327::pollAsync(raw, OBD_QUERY_TIMEOUT);
    if (st == 0) return;  // ancora in attesa: il loop resta libero

    PollItem& it = pollList[pollIdx];
    bool ok = false;
    if (st == 1) {
      ElmResponse r = BleElm327::parseResponse(raw, awaitingHdr);
      ok = r.ok;
      if (ok) {
        if (it.mode == 0x01) {
          applyMode01(data, (uint8_t)it.pid, decodeMode01((uint8_t)it.pid, r));
        } else {
          const auto& p = ProfileLoader::getExtendedPIDs()[it.extIdx];
          OBDValue v = p.structured ? decodeStructured(p, r)
                                    : decodeMode22(p.name, p.unit, p.formula, r);
          if (v.valid) {
            switch (p.target) {
              case ProfileLoader::TARGET_TRANS_TEMP: data.transTemp = v; break;
              case ProfileLoader::TARGET_BOOST:      data.boost     = v; break;
              case ProfileLoader::TARGET_OIL_TEMP:   data.oilTemp   = v; break;
              case ProfileLoader::TARGET_HV_SOC:     data.hvSoc     = v; break;
              case ProfileLoader::TARGET_HV_TEMP:    data.hvTemp    = v; break;
              default: break;
            }
          }
        }
      }
      Serial.printf("[OBD] %s -> %s (ok=%d)\n",
                    BleElm327::lastCmd().c_str(), raw.c_str(), ok);
    } else {
      Serial.printf("[OBD] %s -> timeout\n", BleElm327::lastCmd().c_str());
    }

    if (ok) {
      it.fails = 0;
    } else if (++it.fails >= OBD_PID_MAX_FAILS) {
      it.suspendedUntil = millis() + OBD_PID_RETRY_MS;
      it.fails = 0;
      Serial.printf("[OBD] PID mode=%02X pid=%04X sospeso (%d fail di fila)\n",
                    it.mode, it.pid, OBD_PID_MAX_FAILS);
      if (allFastPidsSuspended()) {
        standby = true;
        lastStandbyProbeMs = millis();  // primo probe tra OBD_STANDBY_PROBE_MS
        Serial.println("[OBD] ECU muta su tutti i PID -> STANDBY (probe ogni 10s)");
      }
    }

    awaiting = false;
    pollIdx++;
    data.lastUpdateMs = millis();
    data.anyValid = data.iat.valid || data.boost.valid ||
                    data.coolant.valid || data.rpm.valid;
  }

  // ── Invia la prossima query eleggibile ───────────────────
  for (size_t scan = 0; scan < pollList.size() && !awaiting; scan++) {
    if (pollIdx >= pollList.size()) { pollIdx = 0; roundNum++; }
    PollItem& it = pollList[pollIdx];

    if (it.suspendedUntil && millis() >= it.suspendedUntil)
      it.suspendedUntil = 0;  // pausa scaduta → riprova
    bool eligible = it.suspendedUntil == 0 && (roundNum % it.every) == 0;
    if (!eligible) { pollIdx++; continue; }

    // Header giusto per questa ECU? Se no, accoda ATSH/ATCRA e riprova
    // questo stesso item al prossimo tick (pollIdx non avanza).
    const char* want = itemHdr(it);
    if (strcmp(want, BleElm327::activeHdr()) != 0) {
      const char* rax = "", *session = "", *fc = "";
      if (it.extIdx >= 0) {
        const auto& ep = ProfileLoader::getExtendedPIDs()[it.extIdx];
        rax = ep.rax; session = ep.session; fc = ep.fc;
      }
      if (queueHeaderSwitch(want, rax, session, fc)) return;
    }

    // Suffisso "risposte attese" (datasheet ELM327): con N note l'ELM
    // ritorna appena ricevute, senza aspettare il timeout del bus.
    // NIENTE suffisso sulle risposte multi-frame (verrebbero troncate).
    char cmd[12];
    int n = BleElm327::getEcuCount();
    if (it.mode == 0x01) {
      if (n >= 1 && n <= 9)
        snprintf(cmd, sizeof(cmd), "%02X%02X%d", it.mode, (uint8_t)it.pid, n);
      else
        snprintf(cmd, sizeof(cmd), "%02X%02X", it.mode, (uint8_t)it.pid);
      snprintf(awaitingHdr, sizeof(awaitingHdr), "%02X%02X",
               it.mode + 0x40, (uint8_t)it.pid);
    } else {
      const auto& p = ProfileLoader::getExtendedPIDs()[it.extIdx];
      bool singleFrame = !p.structured || (uint16_t)(p.bix + p.len) <= 32;
      if (it.mode == 0x22) {
        // Mode 22: PID a 4 hex (UDS DID)
        snprintf(cmd, sizeof(cmd), singleFrame ? "%02X%04X1" : "%02X%04X",
                 it.mode, it.pid);
        snprintf(awaitingHdr, sizeof(awaitingHdr), "%02X%04X",
                 it.mode + 0x40, it.pid);
      } else {
        // Mode 21 (Toyota legacy) e simili: PID a 2 hex
        snprintf(cmd, sizeof(cmd), singleFrame ? "%02X%02X1" : "%02X%02X",
                 it.mode, (uint8_t)it.pid);
        snprintf(awaitingHdr, sizeof(awaitingHdr), "%02X%02X",
                 it.mode + 0x40, (uint8_t)it.pid);
      }
    }

    if (BleElm327::sendAsync(cmd)) awaiting = true;
    else break;  // BLE non pronto: riprova al prossimo tick
  }
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
