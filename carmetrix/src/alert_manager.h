#pragma once
#include <Arduino.h>
#include "obd_decoder.h"

// ============================================================
//  CarMetrix — alert_manager.h
//  Soglie configurabili + buzzer PWM + flash schermo
// ============================================================

enum AlertLevel { ALERT_NONE, ALERT_WARN, ALERT_DANGER };

struct AlertThreshold {
  char  pidName[24];
  float warnVal;
  float dangerVal;
  bool  buzzerEnabled;
};

// Configurazione buzzer (persistita in /buzzer.json)
struct BuzzerConfig {
  bool enabled;
  int  warnFreq;          // Hz tono warn
  int  dangerFreq;        // Hz tono danger
  char dangerStyle[12];   // "continuo" | "beep" | "sirena"
};

namespace AlertManager {
  void begin();                    // init PWM buzzer
  void loadThresholds();           // carica da /alerts.json
  void saveThresholds();           // salva su /alerts.json
  void loadBuzzerCfg();            // carica da /buzzer.json
  const BuzzerConfig& buzzerCfg();

  // Test buzzer dal web (preview tono per trovare il più forte/acuto)
  void requestTest(int freq);      // richiesto dall'endpoint web
  void serviceTest();              // eseguito dal main loop

  // Valuta tutti i valori OBD e triggera feedback
  // Restituisce il livello di allerta più alto trovato
  AlertLevel evaluate(const OBDData& data);

  // Feedback sonori
  void beepConfirm();              // 1 beep corto — azione confermata
  void beepWarn();                 // 2 beep medi
  void beepDanger();               // beep rapidi continui
  void beepError();                // tono discendente — errore
  void beepStop();                 // ferma buzzer

  // Flag per il display — reset automatico dopo il render
  bool shouldFlash();
  AlertLevel currentLevel();
}
