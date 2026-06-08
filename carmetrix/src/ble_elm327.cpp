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
static uint8_t                  detectedProtocol = 0;
static unsigned long            lastReconnectMs  = 0;

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

  dataReady = false;
  rxBuffer  = "";
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

bool BleElm327::init() {
  Serial.println("[ELM] Init...");
  delay(100);
  sendCommand("ATZ",   3000);  // reset — risposta lenta
  delay(500);
  sendCommand("ATE0");  // echo off
  sendCommand("ATL0");  // linefeeds off
  sendCommand("ATS0");  // spaces off (risposta più compatta)
  sendCommand("ATH0");  // headers off
  auto r = sendCommand("ATSP0");  // auto-detect protocollo
  if (!r.ok) { Serial.println("[ELM] Init fallita"); return false; }
  Serial.println("[ELM] Init OK");
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

  // Estrai coppie hex valide come byte
  for (int i = dataStart; i + 1 < (int)s.length() && outLen < 8; i += 2) {
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
