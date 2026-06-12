#pragma once
#include <Arduino.h>
#include "obd_decoder.h"
#include "alert_manager.h"

// ============================================================
//  CarMetrix — display_oled.h
//  Renderer OLED SSD1306 128x64 bicolore (U8g2)
//  Schermate dinamiche: registry PID standard, set attivo e ordine
//  configurabili dalla web app (persistiti in /screens.json).
//  Quando arriverà il TFT, questo file resta invariato
//  e si aggiunge display_tft.h con la stessa interfaccia
// ============================================================

namespace DisplayOled {
  void begin();

  // ── Registry schermate (PID standard già decodificati) ────
  struct ScreenInfo { const char* key; const char* name; const char* unit; };
  int        availableCount();          // schermate nel registry
  ScreenInfo availableScreen(int i);
  int        activeCount();             // schermate attive (>=1)
  const char* activeKey(int i);
  void       loadScreens();             // (ri)legge /screens.json e applica

  // Schermate dati: scrIdx = indice nella lista attiva
  void showData(const OBDData& data, int scrIdx, AlertLevel alert);
  void nextScreen();
  void setScreen(int idx);
  int  currentScreen();

  // Schermate di sistema
  void showBoot();
  void showConfigMode(const char* ssid, const char* ip);
  void showConnecting(const char* deviceName);
  void showWaiting();       // BLE connesso ma nessun dato OBD
  void showNoConnection();  // BLE disconnesso
  void showFlash();         // frame bianco per alert danger
  void showMessage(const char* line1, const char* line2 = nullptr);
  void showResetConfirm(unsigned long heldMs);  // conferma factory reset
}
