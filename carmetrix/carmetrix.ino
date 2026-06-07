// ============================================================
//  CarMetrix v0.1.0
//  Display OBD2 universale — ESP32-S3
//
//  Librerie richieste (Library Manager):
//    - U8g2 by oliver
//    - ESPAsyncWebServer by lacamera (+ AsyncTCP)
//    - ArduinoJson by bblanchon
//    - ESP32 BLE Arduino (incluso nel core ESP32)
//
//  Board: ESP32S3 Dev Module  |  Partition: "Default 4MB with spiffs"
//  Dopo il build: Tools → ESP32 Sketch Data Upload (LittleFS)
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

  // Pressione lunga → entra/esci CONFIG MODE
  if (st == LOW && !btnLongFired &&
      millis() - btnPressedMs > BTN_LONG_PRESS) {
    btnLongFired = true;
    if (appState == STATE_MONITORING || appState == STATE_BLE_CONNECT) {
      // Entra in config mode
      BleElm327::disconnect();
      WiFiPortal::begin();
      WebServer::begin();
      appState = STATE_CONFIG;
      AlertManager::beepConfirm();
    } else if (appState == STATE_DEMO) {
      // In demo AP + web sono già attivi → passa solo alla schermata config
      appState = STATE_CONFIG;
      AlertManager::beepConfirm();
    } else if (appState == STATE_CONFIG) {
      // Esci da config mode senza salvare
      WebServer::stop();
      WiFiPortal::stop();
      appState = STATE_BLE_CONNECT;
      bleConnectStartMs = millis();
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

  if (cfg.demoMode) {
    // Demo Mode: AP + web attivi, dati sintetici, nessun BLE
    Serial.println("[Main] DEMO MODE");
    OBDDecoder::reset(obdData);
    WiFiPortal::begin();
    WebServer::begin();
    appState = STATE_DEMO;
  } else if (!NVSConfig::isConfigured()) {
    // Prima volta: avvia config mode
    Serial.println("[Main] Prima configurazione");
    WiFiPortal::begin();
    WebServer::begin();
    appState = STATE_CONFIG;
  } else {
    // Config esistente: carica profilo e vai in connessione
    Serial.printf("[Main] Config OK: %s / %s\n", cfg.carBrand, cfg.bleMac);
    ProfileLoader::load(cfg.carProfile);
    appState = STATE_BLE_CONNECT;
    bleConnectStartMs = millis();
  }
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  handleButton();

  switch (appState) {

    // ── CONFIG MODE ─────────────────────────────────────────
    case STATE_CONFIG:
      WiFiPortal::loop();
      if (millis() - lastDisplayMs > 1000) {
        lastDisplayMs = millis();
        DisplayOled::showConfigMode(
          WiFiPortal::getSSID().c_str(),
          WiFiPortal::getIP().c_str()
        );
      }
      break;

    // ── CONNESSIONE BLE ──────────────────────────────────────
    case STATE_BLE_CONNECT: {
      NVSConfig::load(cfg);

      // Refresh display ogni 500ms con animazione
      if (millis() - lastDisplayMs > 500) {
        lastDisplayMs = millis();
        DisplayOled::showConnecting(cfg.bleMac[0] ? cfg.bleMac : "Searching...");
      }

      // Tentativo connessione
      if (cfg.bleMac[0] != '\0') {
        if (BleElm327::connect(cfg.bleMac)) {
          if (BleElm327::init()) {
            // Leggi PID supportati e salvali
            uint32_t m1 = BleElm327::getSupportedPIDs(0);
            uint32_t m2 = BleElm327::getSupportedPIDs(1);
            uint32_t m3 = BleElm327::getSupportedPIDs(2);
            NVSConfig::setPidMasks(m1, m2, m3);
            OBDDecoder::reset(obdData);
            AlertManager::beepConfirm();
            appState = STATE_MONITORING;
            Serial.println("[Main] → MONITORING");
          }
        }
      }

      // Timeout → rimane in attesa, riprova ogni 3s via autoReconnectLoop
      BleElm327::autoReconnectLoop();
      delay(100);
      break;
    }

    // ── DEMO MODE ────────────────────────────────────────────
    case STATE_DEMO: {
      WiFiPortal::loop();                  // mantiene captive portal + web
      OBDDecoder::generateDemo(obdData);   // dati sintetici
      AlertLevel alert = AlertManager::evaluate(obdData);
      if (millis() - lastDisplayMs > 80) {
        lastDisplayMs = millis();
        DisplayOled::showData(obdData, DisplayOled::currentScreen(), alert);
      }
      break;
    }

    // ── MONITORING ───────────────────────────────────────────
    case STATE_MONITORING: {
      // Verifica connessione
      if (!BleElm327::isConnected()) {
        DisplayOled::showNoConnection();
        BleElm327::autoReconnectLoop();
        if (BleElm327::isConnected()) {
          BleElm327::init();
          AlertManager::beepConfirm();
        }
        delay(50);
        break;
      }

      // Poll dati OBD
      OBDDecoder::pollAll(obdData, ProfileLoader::hasExtended());

      // Valuta alert
      AlertLevel alert = AlertManager::evaluate(obdData);

      // Render display
      if (millis() - lastDisplayMs > 80) {  // ~12fps
        lastDisplayMs = millis();
        DisplayOled::showData(obdData, DisplayOled::currentScreen(), alert);
      }
      break;
    }

    default: break;
  }
}
