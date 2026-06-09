#pragma once
#include <Arduino.h>
#include <vector>

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
  String  raw;       // risposta grezza
  uint8_t bytes[8];  // byte dati parsati
  uint8_t len;       // numero byte validi
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
}
