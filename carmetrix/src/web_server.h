#pragma once
#include <Arduino.h>
#include "obd_decoder.h"

// ============================================================
//  CarMetrix — web_server.h
//  HTTP server: pagina config + API REST + OTA upload + live data
// ============================================================

namespace WebServer {
  void begin();
  void stop();

  // Callback chiamata dal server quando l'utente salva la config
  // Il main può registrare una funzione da eseguire al salvataggio
  void onConfigSaved(void (*cb)());

  // Sorgente dati live per la dashboard (/api/live)
  // Il main passa il puntatore alla sua struttura OBDData
  void setLiveData(OBDData* data);
}
