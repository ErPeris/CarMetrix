#pragma once
#include <Arduino.h>

// ============================================================
//  CarMetrix — web_server.h
//  HTTP server: pagina config + API REST + OTA upload
// ============================================================

namespace WebServer {
  void begin();
  void stop();

  // Callback chiamata dal server quando l'utente salva la config
  // Il main può registrare una funzione da eseguire al salvataggio
  void onConfigSaved(void (*cb)());
}
