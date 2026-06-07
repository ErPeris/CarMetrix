#pragma once
#include <Arduino.h>

// ============================================================
//  CarMetrix — github_ota.h
//  OTA lato-dispositivo: l'ESP si collega al WiFi di casa (AP+STA),
//  interroga le Release di GitHub e si auto-flasha.
//
//  Flusso: l'endpoint web chiama request(); il main loop chiama run()
//  (bloccante). La web UI segue lo stato via state()/progress()/message().
// ============================================================

namespace GithubOTA {
  enum State {
    IDLE,
    CONNECTING,    // connessione al WiFi di casa
    CHECKING,      // query release GitHub
    NO_UPDATE,     // già aggiornato
    DOWNLOADING,   // download + flash in corso
    FAILED,
    SUCCESS        // (in pratica riavvia prima)
  };

  void        request();         // richiede un OTA (da endpoint web)
  bool        isRequested();
  void        run();             // esegue l'OTA — bloccante, dal main loop

  State       state();
  int         progress();        // 0..100
  const char* message();
  const char* latestVersion();
}
