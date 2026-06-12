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
  char dangerStyle[12];   // "continuo" | "impulsi" | "bitonale" | "allarme"
};

namespace AlertManager {
  void begin();                    // init PWM buzzer
  void loadThresholds();           // carica da /alerts.json
  void saveThresholds();           // salva su /alerts.json
  void loadBuzzerCfg();            // carica da /buzzer.json
  const BuzzerConfig& buzzerCfg();

  // Test buzzer dal web (preview tono per trovare il più forte/acuto).
  // Con style != "" suona il pattern danger di quello stile invece del tono fisso.
  void requestTest(int freq, const char* style = nullptr);
  void serviceTest();              // eseguito dal main loop

  // Valuta tutti i valori OBD e triggera feedback
  // Restituisce il livello di allerta più alto trovato
  AlertLevel evaluate(const OBDData& data);

  // Feedback sonori
  void beepBoot();                 // suono di avvio alla risonanza del buzzer
  void beepConfirm();              // 1 beep corto — azione confermata
  void beepWarn();                 // 2 beep medi
  void beepDanger();               // beep rapidi continui
  void beepError();                // tono discendente — errore
  void beepStop();                 // ferma buzzer

  // Flag per il display — reset automatico dopo il render
  bool shouldFlash();
  AlertLevel currentLevel();
}
