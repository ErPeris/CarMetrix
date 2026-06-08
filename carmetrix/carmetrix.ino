// ============================================================
//  CarMetrix — Display OBD2 universale (ESP32-C3 / S3)
//
//  Librerie (Library Manager):
//    - U8g2 by oliver
//    - ESPAsyncWebServer + AsyncTCP (by ESP32Async)
//    - ArduinoJson by bblanchon
//    - ESP32 BLE Arduino (incluso nel core ESP32 v3.x)
//
//  Board: ESP32C3 Dev Module
//  Partition: "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"
//  Web UI embedded nel firmware (vedi tools/embed_web.py).
//  LittleFS solo per profili veicolo + config runtime.
// ============================================================

#include "src/config.h"
#include "src/nvs_config.h"
#include "src/wifi_portal.h"
#include "src/web_server.h"
#include "src/ble_elm327.h"
#include "src/obd_decoder.h"
#include "src/profile_loader.h"
#include "src/alert_manager.h"
#include "src/display_oled.h"
#include "src/github_ota.h"
#include <LittleFS.h>

// ── State machine ─────────────────────────────────────────────
enum AppState {
  STATE_BOOT,
  STATE_CONFIG,        // hotspot WiFi + web UI
  STATE_BLE_SCAN,      // cerca adattatore BLE
  STATE_BLE_CONNECT,   // connessione in corso
  STATE_MONITORING,    // operatività normale
  STATE_DEMO,          // dati sintetici + web UI (test senza OBD)
};

static AppState appState = STATE_BOOT;
static OBDData  obdData;
static CarMetrixConfig cfg;

// ── Bottone ───────────────────────────────────────────────────
static int  btnLast    = HIGH;
static unsigned long btnPressedMs = 0;
static bool btnLongFired = false;

// ── Timing ───────────────────────────────────────────────────
static unsigned long lastDisplayMs = 0;
static unsigned long bleConnectStartMs = 0;

// ============================================================
//  BUTTON HANDLER
// ============================================================
static void handleButton() {
  int st = digitalRead(PIN_BUTTON);

  // Pressione inizia
  if (st == LOW && btnLast == HIGH) {
    btnPressedMs  = millis();
    btnLongFired  = false;
  }

  // Pressione lunga → entra/esci dalla schermata CONFIG.
  // L'hotspot + web sono SEMPRE attivi: qui cambia solo cosa fa il device.
  if (st == LOW && !btnLongFired &&
      millis() - btnPressedMs > BTN_LONG_PRESS) {
    btnLongFired = true;
    if (appState != STATE_CONFIG) {
      // Entra in CONFIG: ferma i tentativi BLE, mostra schermata setup
      BleElm327::disconnect();
      appState = STATE_CONFIG;
      AlertManager::beepConfirm();
    } else {
      // Esci da CONFIG: riprendi l'attività
      if (cfg.demoMode)         appState = STATE_DEMO;
      else if (cfg.bleMac[0])   { appState = STATE_BLE_CONNECT; bleConnectStartMs = millis(); }
      // (se non configurato resta in CONFIG)
      AlertManager::beepConfirm();
    }
  }

  // Click corto in MONITORING / DEMO → cambia schermata
  if (st == HIGH && btnLast == LOW &&
      millis() - btnPressedMs < BTN_LONG_PRESS &&
      millis() - btnPressedMs > BTN_DEBOUNCE) {
    if (appState == STATE_MONITORING || appState == STATE_DEMO) {
      DisplayOled::nextScreen();
    }
  }

  btnLast = st;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  LittleFS.begin(true);   // monta il FS prima di leggere alert/buzzer config
  AlertManager::begin();
  DisplayOled::begin();
  DisplayOled::showBoot();

  NVSConfig::load(cfg);

  // Callback: quando l'utente salva dal web UI → riavvia
  WebServer::onConfigSaved([]() {
    DisplayOled::showMessage("SALVATO", "Riavvio...");
    delay(800);
    ESP.restart();
  });

  // La dashboard web legge sempre da questa struttura
  WebServer::setLiveData(&obdData);

  // Hotspot + web server SEMPRE attivi, in ogni stato → mai tagliato fuori.
  // Il server è asincrono (task separato): risponde anche durante i
  // tentativi di connessione BLE bloccanti.
  WiFiPortal::begin();
  WebServer::begin();

  if (cfg.demoMode) {
    Serial.println("[Main] DEMO MODE");
    OBDDecoder::reset(obdData);
    appState = STATE_DEMO;
  } else if (!NVSConfig::isConfigured()) {
    Serial.println("[Main] Prima configurazione");
    appState = STATE_CONFIG;
  } else {
    Serial.printf("[Main] Config OK: %s / %s\n", cfg.carBrand, cfg.bleMac);
    ProfileLoader::load(cfg.carProfile);
    appState = STATE_BLE_CONNECT;
    bleConnectStartMs = millis();
  }
}

// ── OTA GitHub: eseguito nel loop (bloccante) quando richiesto ──
static void serviceGithubOTA() {
  if (!GithubOTA::isRequested()) return;
  DisplayOled::showMessage("AGGIORNAMENTO", "via GitHub...");
  GithubOTA::run();   // bloccante; se va a buon fine riavvia da solo
  // Se ritorna qui → fallito o nessun update: mostra l'esito
  DisplayOled::showMessage("OTA", GithubOTA::message());
  delay(2000);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  handleButton();

  // AP/web SEMPRE attivi → questi girano in qualunque stato
  WiFiPortal::loop();          // captive portal DNS
  serviceGithubOTA();          // agisce solo se richiesto via web
  AlertManager::serviceTest(); // agisce solo se richiesto via web

  switch (appState) {

    // ── CONFIG ──────────────────────────────────────────────
    case STATE_CONFIG:
      if (millis() - lastDisplayMs > 1000) {
        lastDisplayMs = millis();
        DisplayOled::showConfigMode(
          WiFiPortal::getSSID().c_str(),
          WiFiPortal::getIP().c_str()
        );
      }
      break;

    // ── CONNESSIONE BLE (web resta raggiungibile) ────────────
    case STATE_BLE_CONNECT: {
      if (millis() - lastDisplayMs > 500) {
        lastDisplayMs = millis();
        DisplayOled::showConnecting(cfg.bleMac[0] ? cfg.bleMac : "Searching...");
      }

      // Tenta connessione + init (throttle 3s interno)
      BleElm327::autoReconnectLoop();

      if (BleElm327::isConnected()) {
        uint32_t m1 = BleElm327::getSupportedPIDs(0);
        uint32_t m2 = BleElm327::getSupportedPIDs(1);
        uint32_t m3 = BleElm327::getSupportedPIDs(2);
        NVSConfig::setPidMasks(m1, m2, m3);
        OBDDecoder::reset(obdData);
        AlertManager::beepConfirm();
        appState = STATE_MONITORING;
        Serial.println("[Main] → MONITORING");
      }
      break;
    }

    // ── DEMO ─────────────────────────────────────────────────
    case STATE_DEMO: {
      OBDDecoder::generateDemo(obdData);
      AlertLevel alert = AlertManager::evaluate(obdData);
      if (millis() - lastDisplayMs > 80) {
        lastDisplayMs = millis();
        DisplayOled::showData(obdData, DisplayOled::currentScreen(), alert);
      }
      break;
    }

    // ── MONITORING ───────────────────────────────────────────
    case STATE_MONITORING: {
      if (!BleElm327::isConnected()) {
        // Connessione persa → torna a riconnettere (web resta su)
        appState = STATE_BLE_CONNECT;
        break;
      }
      OBDDecoder::pollAll(obdData, ProfileLoader::hasExtended());
      AlertLevel alert = AlertManager::evaluate(obdData);
      if (millis() - lastDisplayMs > 80) {  // ~12fps
        lastDisplayMs = millis();
        DisplayOled::showData(obdData, DisplayOled::currentScreen(), alert);
      }
      break;
    }

    default: break;
  }
}
