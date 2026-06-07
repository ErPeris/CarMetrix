#pragma once
#include <Arduino.h>
#include "obd_decoder.h"
#include "alert_manager.h"

// ============================================================
//  CarMetrix — display_oled.h
//  Renderer OLED SSD1306 128x64 bicolore (U8g2)
//  Quando arriverà il TFT, questo file resta invariato
//  e si aggiunge display_tft.h con la stessa interfaccia
// ============================================================

enum OledScreen { SCR_IAT = 0, SCR_BOOST, SCR_TRANS, SCR_RPM, SCR_COUNT };

namespace DisplayOled {
  void begin();

  // Schermate normali
  void showData(const OBDData& data, OledScreen scr, AlertLevel alert);
  void nextScreen();

  // Schermate di sistema
  void showBoot();
  void showConfigMode(const char* ssid, const char* ip);
  void showConnecting(const char* deviceName);
  void showWaiting();       // BLE connesso ma nessun dato OBD
  void showNoConnection();  // BLE disconnesso
  void showFlash();         // frame bianco per alert danger
  void showMessage(const char* line1, const char* line2 = nullptr);

  OledScreen currentScreen();
}
