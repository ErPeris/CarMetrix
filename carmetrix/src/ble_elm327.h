#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

// ============================================================
//  CarMetrix — ble_elm327.h
//  Client BLE per adattatori OBD2 tipo Vgate iCar Pro (ELM327)
// ============================================================

struct BleDevice {
  String name;
  String mac;
  int    rssi;
};

struct ElmResponse {
  bool    ok;
  String  raw;                  // risposta grezza
  uint8_t bytes[ELM_RESP_MAX];  // payload parsato (anche multi-frame)
  uint8_t len;                  // numero byte validi
};

namespace BleElm327 {
  // Scan — restituisce lista dispositivi trovati
  std::vector<BleDevice> scan(uint8_t seconds = 8);

  // Connessione
  bool connect(const char* mac);   // connetti a MAC specifico
  void disconnect();
  bool isConnected();

  // Tenta riconnessione al MAC salvato in NVS (non bloccante)
  void autoReconnectLoop();

  // AT commands
  bool        init();              // ATZ, ATE0, ATL0, ATH0, ATSP0
  ElmResponse sendCommand(const char* cmd, uint32_t timeoutMs = 2000);

  // ── API asincrona (polling non bloccante, stile Car Scanner) ──
  // sendAsync invia e ritorna subito; pollAsync va chiamata dal loop:
  //   0 = risposta in volo, 1 = pronta (out = raw), -1 = timeout/annullata
  bool sendAsync(const char* cmd);
  int  pollAsync(String& out, uint32_t timeoutMs);
  // Parsing di una risposta grezza: hdrHex = header atteso ("410C", "621254")
  ElmResponse parseResponse(const String& raw, const char* hdrHex);
  // Numero di ECU che hanno risposto a 0100 (per il suffisso "risposte
  // attese" nei comandi: l'ELM ritorna subito invece di attendere timeout)
  int  getEcuCount();
  // true se l'init ha agganciato la ECM con header fisico (18DA10F1/7E0):
  // maschera PID completa e Mode 22 possibili, risposte singole
  bool physicalOk();

  // ── Header per-PID (profili schema 2: ogni PID può stare su una ECU
  //    diversa, es. trans temp sul TCM DA1D). Il poller invia i comandi
  //    restituiti qui prima della query, poi aggiorna activeHdr.
  // Riempie out[] con i comandi AT per puntare hdr/rax (per il protocollo
  // attivo); ritorna il numero di comandi (0 se non CAN).
  int  getHeaderCmds(const char* hdr, const char* rax, String out[3]);
  // Comandi per tornare all'header dell'init (fisico ECM o funzionale)
  int  getDefaultHeaderCmds(String out[3]);
  // Bookkeeping dell'header attivo: "" = default dell'init
  const char* activeHdr();
  void setActiveHdr(const char* hdr);

  // PID polling
  ElmResponse queryPID(uint8_t mode, uint8_t pid);   // Mode 01/standard (PID 1 byte)
  ElmResponse queryPID22(uint16_t pid);              // Mode 22 proprietario (PID 2 byte)

  // Query PID supportati (0x0100, 0x0120, 0x0140)
  // Restituisce bitmask 32-bit
  uint32_t getSupportedPIDs(uint8_t group);  // group: 0, 1, 2

  // Protocollo rilevato dopo ATSP0 (0 = non ancora rilevato)
  uint8_t getDetectedProtocol();

  // Diagnostica: ultimo comando inviato e ultima risposta grezza ELM
  String lastCmd();
  String lastRaw();
  bool   initOk();
  bool   ecuOk();    // true se la centralina ha risposto a 0100
  void   setProtocol(const char* p);  // forza ATSP<p> (da profilo); "0" = auto
}
