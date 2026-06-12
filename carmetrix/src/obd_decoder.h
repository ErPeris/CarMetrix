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
  OBDValue oilTemp;    // Oil Temp (profilo)       °C
  OBDValue hvSoc;      // SOC batteria HV (ibride) %
  OBDValue hvTemp;     // Temp batteria HV/inverter °C
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

  // Maschera PID 01-20 supportati (da getSupportedPIDs/0100): i PID
  // assenti non vengono interrogati. 0 = sconosciuta → interroga tutto.
  void setSupportedMask(uint32_t mask1);

  // Polling NON bloccante — chiama dal loop in MONITORING a ogni giro.
  // Tiene una sola query in volo: quando arriva il prompt decodifica e
  // invia subito la successiva. Il loop (display, bottone) non si ferma mai.
  void pollTick(OBDData& data, bool hasExtended = false);

  // true se la ECU non risponde più a nulla (quadro spento): pollTick fa
  // solo un probe 0100 ogni 10s e riparte da solo quando la ECU torna.
  bool isStandby();

  // Demo Mode — genera dati sintetici che spazzano tutte le zone alert
  // (nominale → warn → danger) per testare display/gauge/buzzer senza OBD
  void generateDemo(OBDData& data);

  // Reset dati
  void reset(OBDData& data);
}
