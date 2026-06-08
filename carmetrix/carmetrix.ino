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
static bool beeped3s   = false;   // feedback raggiunti i 3s

// ── Timing ───────────────────────────────────────────────────
static unsigned long lastDisplayMs = 0;
static unsigned long bleConnectStartMs = 0;

// ============================================================
//  BUTTON HANDLER
// ============================================================
// Accende/spegne l'hotspot a seconda dello stato di destinazione.
// AP ON in CONFIG e DEMO; OFF in BLE_CONNECT/MONITORING (radio libera al BLE).
static void enterConfig() {
  BleElm327::disconnect();
  if (appState != STATE_DEMO) { WiFiPortal::begin(); WebServer::begin(); }
  appState = STATE_CONFIG;
  AlertManager::beepConfirm();
}

static void exitConfig() {
  if (cfg.demoMode) {
    appState = STATE_DEMO;                 // demo: AP resta acceso
  } else if (cfg.bleMac[0]) {
    WebServer::stop(); WiFiPortal::stop();  // spegni AP → radio al BLE
    appState = STATE_BLE_CONNECT;
    bleConnectStartMs = millis();
  } else {
    return;  // non configurato: resta in CONFIG
  }
  AlertManager::beepConfirm();
}

static void handleButton() {
  int st = digitalRead(PIN_BUTTON);
  unsigned long now = millis();

  if (st == LOW && btnLast == HIGH) {       // inizio pressione
    btnPressedMs = now;
    beeped3s = false;
  }

  if (st == LOW) {                          // mentre è premuto
    unsigned long held = now - btnPressedMs;
    if (!beeped3s && held >= BTN_LONG_PRESS) {
      beeped3s = true;                      // segnala "config armato"
      AlertManager::beepConfirm();
    }
    if (held >= BTN_FACTORY_RESET) {        // 6s → reset di fabbrica
      DisplayOled::showMessage("FACTORY RESET", "dimentico tutto");
      AlertManager::beepError();
      NVSConfig::reset();
      delay(1000);
      ESP.restart();
    }
  }

  if (st == HIGH && btnLast == LOW) {       // rilascio
    unsigned long held = now - btnPressedMs;
    if (held > BTN_DEBOUNCE && held < BTN_LONG_PRESS) {
      // click corto → cambia schermata
      if (appState == STATE_MONITORING || appState == STATE_DEMO)
        DisplayOled::nextScreen();
    } else if (held >= BTN_LONG_PRESS && held < BTN_FACTORY_RESET) {
      // 3-6s → entra/esci CONFIG
      if (appState == STATE_CONFIG) exitConfig();
      else                          enterConfig();
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

  // L'hotspot si accende solo dove serve (CONFIG/DEMO). Durante il BLE
  // resta SPENTO così l'antenna è tutta per la connessione (C3 mono-radio).
  if (cfg.demoMode) {
    Serial.println("[Main] DEMO MODE");
    OBDDecoder::reset(obdData);
    WiFiPortal::begin();
    WebServer::begin();
    appState = STATE_DEMO;
  } else if (!NVSConfig::isConfigured()) {
    Serial.println("[Main] Prima configurazione");
    WiFiPortal::begin();
    WebServer::begin();
    appState = STATE_CONFIG;
  } else {
    Serial.printf("[Main] Config OK: %s / %s\n", cfg.carBrand, cfg.bleMac);
    ProfileLoader::load(cfg.carProfile);
    appState = STATE_BLE_CONNECT;   // AP spento, radio libera al BLE
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

  // Servizi che richiedono l'AP: solo dove l'hotspot è acceso
  if (appState == STATE_CONFIG || appState == STATE_DEMO) {
    WiFiPortal::loop();          // captive portal DNS
    serviceGithubOTA();          // agisce solo se richiesto via web
    AlertManager::serviceTest(); // agisce solo se richiesto via web
  }

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

    // ── CONNESSIONE BLE (AP spento, radio al BLE) ────────────
    case STATE_BLE_CONNECT: {
      // Timeout: se non si aggancia, riaccende l'hotspot e torna in CONFIG
      if (millis() - bleConnectStartMs > BLE_CONNECT_GIVEUP) {
        Serial.println("[Main] Timeout BLE → CONFIG");
        WiFiPortal::begin();
        WebServer::begin();
        appState = STATE_CONFIG;
        AlertManager::beepError();
        break;
      }

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
        // Connessione persa → torna a riconnettere (riarma il timeout)
        appState = STATE_BLE_CONNECT;
        bleConnectStartMs = millis();
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
