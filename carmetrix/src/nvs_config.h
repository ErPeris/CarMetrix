#pragma once
#include <Arduino.h>

// ============================================================
//  CarMetrix — nvs_config.h
//  Salvataggio persistente in NVS (Preferences)
// ============================================================

struct CarMetrixConfig {
  char     bleMac[18];       // "AA:BB:CC:DD:EE:FF\0"
  char     carBrand[32];     // "Honda"
  char     carProfile[64];   // "honda.json" o "toyota/yaris_hybrid.json"
  uint8_t  obdProtocol;      // 0 = auto-detect
  uint32_t pidMask1;         // PID supportati 01-20
  uint32_t pidMask2;         // PID supportati 21-40
  uint32_t pidMask3;         // PID supportati 41-60
  bool     configured;       // true dopo primo setup completo
  bool     demoMode;         // true → genera dati sintetici, salta BLE
  char     homeSsid[33];     // WiFi di casa per OTA da GitHub
  char     homePass[65];     // password WiFi di casa
};

namespace NVSConfig {
  void        load(CarMetrixConfig& cfg);
  void        save(const CarMetrixConfig& cfg);
  void        reset();
  bool        isConfigured();
  void        setBleMac(const char* mac);
  void        setCarProfile(const char* brand, const char* profileFile);
  void        setObdProtocol(uint8_t proto);
  void        setPidMasks(uint32_t m1, uint32_t m2, uint32_t m3);
}
