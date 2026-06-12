#include "ble_elm327.h"
#include "config.h"
#include "nvs_config.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>
#include <map>

// ── Stato interno ────────────────────────────────────────────
static BLEClient*               bleClient   = nullptr;
static BLERemoteCharacteristic* charWrite   = nullptr;
static BLERemoteCharacteristic* charNotify  = nullptr;
static bool                     writeWithResponse = false;
static volatile bool            connected   = false;
static volatile bool            dataReady   = false;
static String                   rxBuffer    = "";
static String                   lastResponse = "";
static String                   lastCmdSent  = "";
static bool                     elmInitOk    = false;
static bool                     ecuResponding = false;
static String                   forcedProtocol = "0";   // ATSP, "0" = auto
static uint8_t                  detectedProtocol = 0;
static unsigned long            lastReconnectMs  = 0;
static volatile bool            asyncPending = false;
static unsigned long            asyncStartMs = 0;
static int                      ecuCount     = 0;   // ECU che rispondono a 0100
static bool                     physicalAddressing = false;  // header fisico ECM attivo
static String                   currentHdr   = "";  // header attivo ("" = default init)
static String                   dfltCmds[3];        // AT per ripristinare il default
static int                      dfltCmdCount = 0;

// Forward declaration (definita nella sezione PID QUERY)
static bool extractBytes(const String& raw, const String& respHeaderHex,
                         uint8_t* out, uint8_t& outLen);

// ── Callback notifica BLE ────────────────────────────────────
static void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '>') {
      // Prompt ELM327 → risposta completa
      lastResponse = rxBuffer;
      lastResponse.trim();
      rxBuffer = "";
      dataReady = true;
    } else if (c != '\r') {
      rxBuffer += c;
    }
  }
}

// ── Callback disconnessione ──────────────────────────────────
class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*)    override { connected = true;  }
  void onDisconnect(BLEClient*) override {
    connected = false;
    charWrite  = nullptr;
    charNotify = nullptr;
    Serial.println("[BLE] Disconnesso");
  }
};
static ClientCallbacks clientCb;

// ============================================================
//  SCAN
// ============================================================
std::vector<BleDevice> BleElm327::scan(uint8_t seconds) {
  std::vector<BleDevice> results;
  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  BLEScanResults* found = scan->start(seconds, false);

  for (int i = 0; i < found->getCount(); i++) {
    BLEAdvertisedDevice d = found->getDevice(i);
    BleDevice bd;
    bd.name = d.haveName() ? d.getName().c_str() : "(sconosciuto)";
    bd.mac  = d.getAddress().toString().c_str();
    bd.rssi = d.getRSSI();
    // Filtra per nome tipici adattatori OBD2
    String n = bd.name;
    n.toLowerCase();
    if (n.indexOf("icar") >= 0 || n.indexOf("obd") >= 0 ||
        n.indexOf("elm") >= 0 || n.indexOf("vgate") >= 0 ||
        d.haveServiceUUID() /* qualsiasi con servizi */) {
      results.push_back(bd);
    }
  }
  scan->clearResults();
  return results;
}

// ============================================================
//  CONNECT
// ============================================================
bool BleElm327::connect(const char* mac) {
  Serial.printf("[BLE] Connessione a %s...\n", mac);

  if (!bleClient) {
    BLEDevice::init("CarMetrix");
    bleClient = BLEDevice::createClient();
    bleClient->setClientCallbacks(&clientCb);
  }

  // Scan per trovare il dispositivo e usare il suo ADDRESS TYPE corretto.
  // Molti adattatori usano indirizzi "random": connettersi col MAC come
  // "public" fallisce in silenzio. Connettendoci tramite l'oggetto trovato
  // nello scan usiamo il tipo giusto, e verifichiamo che stia trasmettendo.
  Serial.printf("[BLE] Scan per %s...\n", mac);
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  BLEScanResults* found = scan->start(5, false);

  static BLEAdvertisedDevice target;
  bool haveTarget = false;
  String want = String(mac); want.toLowerCase();
  for (int i = 0; i < found->getCount(); i++) {
    BLEAdvertisedDevice d = found->getDevice(i);
    String a = String(d.getAddress().toString().c_str()); a.toLowerCase();
    if (a == want) { target = d; haveTarget = true; break; }
  }
  scan->clearResults();

  if (!haveTarget) {
    Serial.println("[BLE] non in advertising (spento? gia' connesso al telefono?)");
    return false;
  }

  if (!bleClient->connect(&target)) {   // usa l'address type corretto
    Serial.println("[BLE] connect() fallita");
    return false;
  }

  Serial.println("[BLE] Connesso (GAP). Discovery servizi...");

  // ── AUTO-DISCOVERY ───────────────────────────────────────
  // Niente UUID hardcodati: enumeriamo i servizi e troviamo da soli
  // una caratteristica NOTIFY (lettura) e una WRITE (comandi).
  // Funziona con Vgate iCar Pro (18F0/2AF0/2AF1), cloni FFF0/FFF1/FFF2,
  // FFE0/FFE1 e qualunque altra variante ELM327 BLE.
  charWrite  = nullptr;
  charNotify = nullptr;

  std::map<std::string, BLERemoteService*>* services = bleClient->getServices();
  if (services) {
    for (auto& sPair : *services) {
      BLERemoteService* svc = sPair.second;
      Serial.printf("[BLE] Servizio %s\n", svc->getUUID().toString().c_str());

      BLERemoteCharacteristic* w = nullptr;
      BLERemoteCharacteristic* n = nullptr;
      std::map<std::string, BLERemoteCharacteristic*>* chars = svc->getCharacteristics();
      if (chars) {
        for (auto& cPair : *chars) {
          BLERemoteCharacteristic* ch = cPair.second;
          Serial.printf("[BLE]   char %s  [%s%s%s%s]\n",
            ch->getUUID().toString().c_str(),
            ch->canRead()           ? "R" : "",
            ch->canWrite()          ? "W" : "",
            ch->canWriteNoResponse()? "w" : "",
            ch->canNotify()         ? "N" : "");
          if (!n && (ch->canNotify() || ch->canIndicate()))    n = ch;
          if (!w && (ch->canWrite() || ch->canWriteNoResponse())) w = ch;
        }
      }
      if (w && n) { charWrite = w; charNotify = n; break; }
    }
  }

  if (!charWrite || !charNotify) {
    Serial.println("[BLE] Nessun servizio con NOTIFY+WRITE trovato");
    bleClient->disconnect();
    return false;
  }

  // Modalità di scrittura: usa write-with-response solo se supportata
  writeWithResponse = charWrite->canWrite();
  Serial.printf("[BLE] WRITE=%s (resp=%d)  NOTIFY=%s\n",
    charWrite->getUUID().toString().c_str(), writeWithResponse,
    charNotify->getUUID().toString().c_str());

  if (charNotify->canNotify())        charNotify->registerForNotify(notifyCallback, true);
  else if (charNotify->canIndicate()) charNotify->registerForNotify(notifyCallback, false);

  connected = true;
  Serial.println("[BLE] Pronto");
  return true;
}

void BleElm327::disconnect() {
  if (bleClient && bleClient->isConnected())
    bleClient->disconnect();
  connected = false;
}

bool BleElm327::isConnected() {
  return connected && bleClient && bleClient->isConnected();
}

// ── Riconnessione automatica (chiama dal loop) ───────────────
void BleElm327::autoReconnectLoop() {
  if (isConnected()) return;
  unsigned long now = millis();
  if (now - lastReconnectMs < BLE_RECONNECT_EVERY) return;
  lastReconnectMs = now;

  CarMetrixConfig cfg;
  NVSConfig::load(cfg);
  if (cfg.bleMac[0] == '\0') return;

  Serial.println("[BLE] Tentativo riconnessione...");
  if (connect(cfg.bleMac)) init();
}

// ============================================================
//  AT COMMANDS
// ============================================================
ElmResponse BleElm327::sendCommand(const char* cmd, uint32_t timeoutMs) {
  ElmResponse resp;
  resp.ok  = false;
  resp.len = 0;

  if (!isConnected() || !charWrite) return resp;

  asyncPending = false;  // annulla eventuale query async in volo
  dataReady = false;
  rxBuffer  = "";
  lastCmdSent = cmd;
  String s  = String(cmd) + "\r";
  charWrite->writeValue((uint8_t*)s.c_str(), s.length(), writeWithResponse);

  unsigned long t = millis();
  while (!dataReady && millis() - t < timeoutMs) delay(10);

  if (!dataReady) {
    Serial.printf("[ELM] Timeout: %s\n", cmd);
    return resp;
  }

  resp.raw = lastResponse;
  resp.ok  = (resp.raw.indexOf("ERROR") < 0 &&
              resp.raw.indexOf("UNABLE") < 0 &&
              resp.raw.length() > 0);
  return resp;
}

// ── API asincrona: invia e torna subito, il loop interroga pollAsync ──
bool BleElm327::sendAsync(const char* cmd) {
  if (!isConnected() || !charWrite) return false;
  dataReady = false;
  rxBuffer  = "";
  lastCmdSent = cmd;
  String s = String(cmd) + "\r";
  charWrite->writeValue((uint8_t*)s.c_str(), s.length(), writeWithResponse);
  asyncPending = true;
  asyncStartMs = millis();
  return true;
}

int BleElm327::pollAsync(String& out, uint32_t timeoutMs) {
  if (!asyncPending) return -1;  // annullata (es. dal ponte seriale)
  if (dataReady) {
    asyncPending = false;
    out = lastResponse;
    return 1;
  }
  if (millis() - asyncStartMs >= timeoutMs) {
    asyncPending = false;
    return -1;
  }
  return 0;
}

ElmResponse BleElm327::parseResponse(const String& raw, const char* hdrHex) {
  ElmResponse r;
  r.raw = raw;
  r.len = 0;
  r.ok  = extractBytes(raw, String(hdrHex), r.bytes, r.len);
  return r;
}

int BleElm327::getEcuCount() { return ecuCount; }

// Conta quante ECU hanno risposto a 0100 (occorrenze di "4100").
// Serve per il suffisso "risposte attese": es. con 2 ECU "010C2" fa
// tornare l'ELM appena arrivate entrambe, senza attendere il timeout bus.
static int countEcus(const String& raw) {
  String s = raw;
  s.toUpperCase();
  s.replace(" ", ""); s.replace("\r", ""); s.replace("\n", "");
  int n = 0, idx = 0;
  while ((idx = s.indexOf("4100", idx)) >= 0) { n++; idx += 4; }
  return n > 6 ? 6 : n;
}

// ── Physical addressing alla ECM ─────────────────────────────
// Con l'header funzionale broadcast alcune auto (Honda 10th-gen in 29-bit)
// rispondono con un set ridotto di PID e ignorano il Mode 22 (UDS richiede
// indirizzamento fisico). Qui puntiamo direttamente la ECM:
//   29-bit: richiesta 18DA10F1, risposta 18DAF110
//   11-bit: richiesta 7E0,      risposta 7E8
static bool tryPhysicalHeader(uint8_t proto) {
  if (proto == 7 || proto == 9) {
    BleElm327::sendCommand("ATCP18");
    BleElm327::sendCommand("ATSHDA10F1");
    BleElm327::sendCommand("ATCRA18DAF110");  // filtro RX sulla risposta ECM
  } else if (proto == 6 || proto == 8) {
    BleElm327::sendCommand("ATSH7E0");
    BleElm327::sendCommand("ATCRA7E8");
  } else {
    return false;  // non-CAN: physical addressing non gestito
  }
  uint8_t buf[ELM_RESP_MAX]; uint8_t blen = 0;
  auto r = BleElm327::sendCommand("01001", 5000);  // suffisso 1: una sola ECU
  return r.ok && extractBytes(r.raw, "4100", buf, blen);
}

static void restoreFunctionalHeader(uint8_t proto) {
  BleElm327::sendCommand("ATAR");  // annulla il filtro CRA
  if (proto == 7 || proto == 9) {
    BleElm327::sendCommand("ATCP18");
    BleElm327::sendCommand("ATSHDB33F1");
  } else if (proto == 6 || proto == 8) {
    BleElm327::sendCommand("ATSH7DF");
  }
}

bool BleElm327::init() {
  Serial.println("[ELM] Init...");
  physicalAddressing = false;
  delay(100);
  sendCommand("ATZ",   3000);  // reset — risposta lenta
  delay(500);
  sendCommand("ATE0");   // echo off
  sendCommand("ATL0");   // linefeeds off
  sendCommand("ATS0");   // spaces off (risposta più compatta)
  sendCommand("ATH0");   // headers off
  sendCommand("ATAT2");  // adaptive timing aggressivo → meno attesa dopo le risposte

  // Protocollo del profilo (es. Honda "7" = CAN 29bit 500k), default auto
  String sp = "ATSP" + forcedProtocol;
  auto r = sendCommand(sp.c_str());
  elmInitOk = r.ok;
  if (!r.ok) { Serial.println("[ELM] Init fallita"); ecuResponding = false; return false; }

  // CAN 29-bit: i cloni spesso NON impostano da soli l'header funzionale
  // OBD (ISO 15765-4: richiesta 0x18DB33F1). Lo forziamo esplicitamente:
  // ATCP = byte di priorità (18), ATSH = restanti 24 bit (DB33F1).
  if (forcedProtocol == "7" || forcedProtocol == "9") {
    sendCommand("ATCP18");
    sendCommand("ATSHDB33F1");
  }

  // Probe: la centralina risponde? (0100 = PID supportati). Timeout lungo:
  // la prima query su bus freddo / ricerca protocollo può superare i 5s.
  uint8_t buf[ELM_RESP_MAX]; uint8_t blen = 0;
  auto probe = sendCommand("0100", 10000);
  ecuResponding = probe.ok && extractBytes(probe.raw, "4100", buf, blen);

  // Fallback: se il protocollo forzato non risponde, prova l'auto-detect
  if (!ecuResponding && forcedProtocol != "0") {
    Serial.println("[ELM] Protocollo forzato muto → provo ATSP0 auto");
    sendCommand("ATSP0");
    probe = sendCommand("0100", 10000);
    ecuResponding = probe.ok && extractBytes(probe.raw, "4100", buf, blen);
  }

  ecuCount = ecuResponding ? countEcus(probe.raw) : 0;

  // Protocollo effettivo (ATDPN: es. "7", o "A7" se rilevato in auto)
  auto dp = sendCommand("ATDPN");
  String dps = dp.raw; dps.trim(); dps.toUpperCase(); dps.replace("A", "");
  detectedProtocol = (uint8_t)strtol(dps.c_str(), nullptr, 16);
  Serial.printf("[ELM] Protocollo attivo: %d\n", detectedProtocol);

  // Physical addressing alla ECM: maschera PID completa, Mode 22 possibile,
  // risposte singole (suffisso "1" → query ancora più rapide).
  if (ecuResponding && tryPhysicalHeader(detectedProtocol)) {
    physicalAddressing = true;
    ecuCount = 1;
    Serial.println("[ELM] Physical addressing: OK (ECM diretta)");
  } else if (ecuResponding) {
    restoreFunctionalHeader(detectedProtocol);
    Serial.println("[ELM] Physical addressing muto -> fallback funzionale");
  }

  // Registra i comandi per ripristinare QUESTO header (il "default") dopo
  // le query su altre ECU (profili schema 2 con hdr per-PID).
  dfltCmdCount = 0;
  currentHdr = "";
  if (detectedProtocol == 7 || detectedProtocol == 9) {
    if (physicalAddressing) {
      dfltCmds[0] = "ATSHDA10F1"; dfltCmds[1] = "ATCRA18DAF110"; dfltCmdCount = 2;
    } else {
      dfltCmds[0] = "ATAR"; dfltCmds[1] = "ATCP18"; dfltCmds[2] = "ATSHDB33F1"; dfltCmdCount = 3;
    }
  } else if (detectedProtocol == 6 || detectedProtocol == 8) {
    if (physicalAddressing) {
      dfltCmds[0] = "ATSH7E0"; dfltCmds[1] = "ATCRA7E8"; dfltCmdCount = 2;
    } else {
      dfltCmds[0] = "ATAR"; dfltCmds[1] = "ATSH7DF"; dfltCmdCount = 2;
    }
  }

  Serial.printf("[ELM] Init OK (ATSP%s). ECU %s (n=%d%s)\n",
                forcedProtocol.c_str(), ecuResponding ? "risponde" : "MUTA",
                ecuCount, physicalAddressing ? ", fisico" : "");
  return true;
}

// ============================================================
//  PID QUERY
// ============================================================
// ── Parser robusto: estrae i byte dati da una risposta ELM327 ──
// respHeaderHex = header atteso, es. "410C" (Mode01 PID 0x0C) o "621234" (Mode22)
// Gestisce: SEARCHING..., NO DATA, ERROR, spazi (ATS1), CR/LF, prompt '>',
// e risposte multi-frame CAN (righe con prefisso "0:", "1:"...).
static bool extractBytes(const String& raw, const String& respHeaderHex,
                         uint8_t* out, uint8_t& outLen) {
  outLen = 0;
  String s = raw;
  s.toUpperCase();

  // Errori / stati non validi
  if (s.indexOf("NO DATA")  >= 0 || s.indexOf("ERROR")   >= 0 ||
      s.indexOf("UNABLE")   >= 0 || s.indexOf("STOPPED") >= 0 ||
      s.indexOf("BUFFER")   >= 0 || s.indexOf("?")       >= 0) return false;

  // Rimuovi rumore tipico
  s.replace("SEARCHING...", "");
  s.replace("SEARCHING", "");
  s.replace(" ", "");
  s.replace("\r", "");
  s.replace("\n", "");
  s.replace(">", "");
  // Marcatori di frame ISO-TP multi-linea ("0:", "1:", ...) → rimuovi "N:"
  for (char d = '0'; d <= '9'; d++) {
    String marker = String(d) + ":";
    s.replace(marker, "");
  }

  // Trova l'header di risposta (mode+0x40 + pid echo)
  int idx = s.indexOf(respHeaderHex);
  if (idx < 0) return false;
  int dataStart = idx + respHeaderHex.length();

  // Estrai coppie hex valide come byte (multi-frame: fino a ELM_RESP_MAX)
  for (int i = dataStart; i + 1 < (int)s.length() && outLen < ELM_RESP_MAX; i += 2) {
    char c0 = s[i], c1 = s[i + 1];
    if (!isHexadecimalDigit(c0) || !isHexadecimalDigit(c1)) break;
    char bs[3] = { c0, c1, 0 };
    out[outLen++] = (uint8_t)strtol(bs, nullptr, 16);
  }
  return outLen > 0;
}

ElmResponse BleElm327::queryPID(uint8_t mode, uint8_t pid) {
  char cmd[8];
  snprintf(cmd, sizeof(cmd), "%02X%02X", mode, pid);
  ElmResponse resp = sendCommand(cmd, ELM_CMD_TIMEOUT);
  if (!resp.ok) return resp;

  // Header risposta = (mode | 0x40) + pid  → es. 010C → "410C"
  char hdr[8];
  snprintf(hdr, sizeof(hdr), "%02X%02X", mode + 0x40, pid);
  resp.ok = extractBytes(resp.raw, hdr, resp.bytes, resp.len);
  Serial.printf("[OBD] %s -> %s (ok=%d len=%d)\n",
                cmd, resp.raw.c_str(), resp.ok, resp.len);
  return resp;
}

ElmResponse BleElm327::queryPID22(uint16_t pid) {
  char cmd[8];
  snprintf(cmd, sizeof(cmd), "22%04X", pid);
  ElmResponse resp = sendCommand(cmd, ELM_CMD_TIMEOUT);
  if (!resp.ok) return resp;

  // Header risposta Mode 22 = "62" + pid(2 byte)  → es. 221234 → "621234"
  char hdr[8];
  snprintf(hdr, sizeof(hdr), "62%04X", pid);
  resp.ok = extractBytes(resp.raw, hdr, resp.bytes, resp.len);
  Serial.printf("[OBD] %s -> %s (ok=%d len=%d)\n",
                cmd, resp.raw.c_str(), resp.ok, resp.len);
  return resp;
}

uint32_t BleElm327::getSupportedPIDs(uint8_t group) {
  uint8_t pid = group * 0x20;  // 0x00, 0x20, 0x40
  ElmResponse r = queryPID(0x01, pid);
  if (!r.ok || r.len < 4) return 0;
  return ((uint32_t)r.bytes[0] << 24) |
         ((uint32_t)r.bytes[1] << 16) |
         ((uint32_t)r.bytes[2] << 8)  |
          (uint32_t)r.bytes[3];
}

uint8_t BleElm327::getDetectedProtocol() { return detectedProtocol; }

bool BleElm327::physicalOk() { return physicalAddressing; }

// ── Header per-PID (profili schema 2) ────────────────────────
// 29-bit (proto 7/9): hdr OBDb "DA1D" → ATSH DA1DF1, risposta 18DAF1<rax>
// 11-bit (proto 6/8): hdr OBDb "7E0"  → ATSH 7E0,    risposta = rax ("7E8")
int BleElm327::getHeaderCmds(const char* hdr, const char* rax, String out[3]) {
  if (!hdr || !hdr[0]) return getDefaultHeaderCmds(out);
  if (detectedProtocol == 7 || detectedProtocol == 9) {
    out[0] = String("ATSH") + hdr + "F1";
    out[1] = String("ATCRA18DAF1") + (rax && rax[0] ? rax : "");
    return (rax && rax[0]) ? 2 : 1;
  }
  if (detectedProtocol == 6 || detectedProtocol == 8) {
    out[0] = String("ATSH") + hdr;
    out[1] = String("ATCRA") + (rax && rax[0] ? rax : "");
    return (rax && rax[0]) ? 2 : 1;
  }
  return 0;  // non-CAN: header non gestiti
}

int BleElm327::getDefaultHeaderCmds(String out[3]) {
  for (int i = 0; i < dfltCmdCount; i++) out[i] = dfltCmds[i];
  return dfltCmdCount;
}

const char* BleElm327::activeHdr() { return currentHdr.c_str(); }
void BleElm327::setActiveHdr(const char* hdr) { currentHdr = hdr ? hdr : ""; }

String BleElm327::lastCmd() { return lastCmdSent; }
String BleElm327::lastRaw() { return lastResponse; }
bool   BleElm327::initOk()  { return elmInitOk; }
bool   BleElm327::ecuOk()   { return ecuResponding; }
void   BleElm327::setProtocol(const char* p) { forcedProtocol = (p && *p) ? p : "0"; }
