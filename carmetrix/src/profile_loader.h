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
    TARGET_HV_SOC,     // stato carica batteria HV (ibride/EV), %
    TARGET_HV_TEMP,    // temperatura batteria HV / inverter, °C
    TARGET_CUSTOM
  };

  // PID proprietario. Due forme di decodifica:
  //  - schema 2 (structured=true): estrazione bix/len dal payload dopo
  //    l'echo, value = raw*mul/div + add, clamp su mn/mx (formato OBDb)
  //  - schema 1 (structured=false): formula legacy "A-40" ecc.
  struct ExtendedPID {
    char      name[24];
    char      unit[8];
    char      formula[32];   // solo schema 1
    uint8_t   mode;          // 0x22 (Mode 22)
    uint16_t  pidHex;        // es. 0x2201
    char      hdr[8];        // header ECU target ("DA1D", "7E0"); "" = default
    char      rax[6];        // filtro risposta ("1D", "7E8")
    bool      structured;    // true = schema 2
    uint16_t  bix;           // bit offset nel payload dopo l'echo 62+pid
    uint8_t   len;           // lunghezza in bit (max 32)
    bool      sign;
    float     mul, div, add;
    float     mn, mx;        // fuori range → valore scartato
    uint8_t   every;         // interroga ogni N giri
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
