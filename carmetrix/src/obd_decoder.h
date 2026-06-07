#pragma once
#include <Arduino.h>
#include "ble_elm327.h"

// ============================================================
//  CarMetrix — obd_decoder.h
//  Decodifica risposte ELM327 in valori reali
//  Supporta Mode 01 (standard) e Mode 22 (proprietario)
// ============================================================

struct OBDValue {
  float   value;
  bool    valid;
  char    unit[8];
  char    name[24];
};

// Dati OBD correnti (aggiornati dal polling loop)
struct OBDData {
  OBDValue iat;        // Intake Air Temp         °C
  OBDValue coolant;    // Coolant Temperature      °C
  OBDValue map;        // Manifold Absolute Pressure kPa
  OBDValue boost;      // Boost (MAP - 101.3)      bar
  OBDValue transTemp;  // Transmission Temp        °C
  OBDValue rpm;        // RPM
  OBDValue speed;      // Speed                    km/h
  OBDValue throttle;   // Throttle Position        %
  OBDValue load;       // Engine Load              %
  unsigned long lastUpdateMs;
  bool           anyValid;
};

namespace OBDDecoder {
  // Decodifica PID standard Mode 01
  OBDValue decodeMode01(uint8_t pid, const ElmResponse& r);

  // Decodifica PID proprietario Mode 22 con formula custom
  // formula: stringa tipo "A*0.1-101.3"  (A, B = byte 0, 1)
  OBDValue decodeMode22(const char* name, const char* unit,
                        const char* formula, const ElmResponse& r);

  // Poll completo — chiama dal loop in NORMAL MODE
  // Aggiorna la struttura OBDData passata per riferimento
  void pollAll(OBDData& data, bool hasExtended = false);

  // Demo Mode — genera dati sintetici che spazzano tutte le zone alert
  // (nominale → warn → danger) per testare display/gauge/buzzer senza OBD
  void generateDemo(OBDData& data);

  // Reset dati
  void reset(OBDData& data);
}
