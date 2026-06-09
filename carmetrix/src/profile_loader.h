#pragma once
#include <Arduino.h>
#include <vector>

// ============================================================
//  CarMetrix — profile_loader.h
//  Carica profilo veicolo da LittleFS (/profiles/xxx.json)
//  I PID Mode 22 estendono quelli standard Mode 01
// ============================================================

namespace ProfileLoader {

  enum PidTarget {
    TARGET_BOOST,
    TARGET_TRANS_TEMP,
    TARGET_OIL_TEMP,
    TARGET_TURBO_RPM,
    TARGET_CUSTOM
  };

  struct ExtendedPID {
    char      name[24];
    char      unit[8];
    char      formula[32];
    uint16_t  pidHex;      // es. 0x1754
    PidTarget target;
  };

  // Carica profilo — chiama al boot o quando l'utente cambia veicolo
  bool load(const char* profileFile);
  void unload();

  // Accesso dati caricati
  const char*                    getBrand();
  const char*                    getProtocol();   // ATSP, "0" = auto
  const std::vector<ExtendedPID>& getExtendedPIDs();
  bool                           hasExtended();
}
