#pragma once
#include <Arduino.h>

// ============================================================
//  CarMetrix — wifi_portal.h
//  Access Point + Captive Portal DNS
//  Il telefono si connette e il browser si apre automaticamente
// ============================================================

namespace WiFiPortal {
  void begin();   // avvia AP + DNS server
  void loop();    // chiama nel loop principale quando in CONFIG MODE
  void stop();    // spegne AP e DNS
  String getSSID();
  String getIP();
}
