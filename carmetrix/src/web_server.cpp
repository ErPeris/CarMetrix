#include "web_server.h"
#include "config.h"
#include "nvs_config.h"
#include "ble_elm327.h"
#include "github_ota.h"
#include "alert_manager.h"
#include "web_index.h"        // web UI compressa (embedded → OTA)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>           // AsyncCallbackJsonWebHandler
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>

static AsyncWebServer server(WEB_PORT);
static void (*savedCallback)() = nullptr;
static bool      otaInProgress = false;
static OBDData*  liveData      = nullptr;

// ── Helper: risposta JSON ────────────────────────────────────
static void jsonReply(AsyncWebServerRequest* req, int code, const String& body) {
  req->send(code, "application/json", body);
}

// ── Redirect captive portal ──────────────────────────────────
static void handleCaptive(AsyncWebServerRequest* req) {
  req->redirect("http://192.168.4.1/");
}

// ============================================================
void WebServer::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[Web] LittleFS mount failed");
  }

  // ── Captive portal triggers (iOS, Android, Windows) ──────
  server.on("/hotspot-detect.html",       HTTP_GET, handleCaptive);
  server.on("/generate_204",              HTTP_GET, handleCaptive);
  server.on("/connecttest.txt",           HTTP_GET, handleCaptive);
  server.on("/ncsi.txt",                  HTTP_GET, handleCaptive);
  server.on("/redirect",                  HTTP_GET, handleCaptive);
  server.on("/canonical.html",            HTTP_GET, handleCaptive);
  server.on("/success.txt",               HTTP_GET, handleCaptive);

  // ── Pagina principale: embedded nel firmware (gzip) ──────
  //  Così la UI si aggiorna via OTA insieme al firmware.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* resp =
      req->beginResponse(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    resp->addHeader("Content-Encoding", "gzip");
    req->send(resp);
  });

  // ── File statici (js, css, icons) ────────────────────────
  server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=600");

  // ─────────────────────────────────────────────────────────
  //  API: scan BLE adapters
  // ─────────────────────────────────────────────────────────
  server.on("/api/ble/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    // Avvia scan e risponde con la lista trovata
    auto results = BleElm327::scan(BLE_SCAN_DURATION);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (auto& r : results) {
      JsonObject obj = arr.add<JsonObject>();
      obj["name"] = r.name;
      obj["mac"]  = r.mac;
      obj["rssi"] = r.rssi;
    }
    String out;
    serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: scan reti WiFi (async) — trigger
  // ─────────────────────────────────────────────────────────
  server.on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest* req) {
    WiFi.mode(WIFI_AP_STA);     // abilita STA mantenendo l'AP
    WiFi.scanDelete();
    WiFi.scanNetworks(true);    // asincrono, non blocca
    jsonReply(req, 200, "{\"ok\":true}");
  });

  // ─────────────────────────────────────────────────────────
  //  API: risultati scan WiFi (polling)
  // ─────────────────────────────────────────────────────────
  server.on("/api/wifi/scan/results", HTTP_GET, [](AsyncWebServerRequest* req) {
    int n = WiFi.scanComplete();
    JsonDocument doc;
    if (n == WIFI_SCAN_RUNNING) {
      doc["scanning"] = true;
    } else {
      doc["scanning"] = false;
      JsonArray arr = doc["nets"].to<JsonArray>();
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;   // scarta reti nascoste
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["lock"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      if (n >= 0) WiFi.scanDelete();
    }
    String out; serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: lista profili veicolo disponibili
  // ─────────────────────────────────────────────────────────
  server.on("/api/profiles", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    File dir = LittleFS.open("/profiles");
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String fname = f.name();
          // Leggi il campo "brand" dal JSON del profilo
          JsonDocument tmp;
          DeserializationError err = deserializeJson(tmp, f);
          if (!err) {
            JsonObject obj = arr.add<JsonObject>();
            obj["file"]  = fname;
            obj["brand"] = tmp["brand"] | fname;
          }
        }
        f = dir.openNextFile();
      }
    }
    String out;
    serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: configurazione corrente
  // ─────────────────────────────────────────────────────────
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    CarMetrixConfig cfg;
    NVSConfig::load(cfg);
    JsonDocument doc;
    doc["bleMac"]      = cfg.bleMac;
    doc["carBrand"]    = cfg.carBrand;
    doc["carProfile"]  = cfg.carProfile;
    doc["configured"]  = cfg.configured;
    doc["demoMode"]    = cfg.demoMode;
    doc["version"]     = CARMETRIX_VERSION;
    doc["ghOwner"]     = GITHUB_OWNER;
    doc["ghRepo"]      = GITHUB_REPO;
    doc["homeSsid"]    = cfg.homeSsid;
    doc["hasWifiPass"] = (cfg.homePass[0] != '\0');  // non esporre la password
    String out;
    serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: dati live (dashboard) — legge la struttura del main
  // ─────────────────────────────────────────────────────────
  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    if (liveData) {
      auto add = [&](const char* key, const OBDValue& v) {
        JsonObject o = doc[key].to<JsonObject>();
        o["v"]     = round(v.value * 100) / 100.0;
        o["valid"] = v.valid;
      };
      add("iat",     liveData->iat);
      add("boost",   liveData->boost);
      add("trans",   liveData->transTemp);
      add("coolant", liveData->coolant);
      add("rpm",     liveData->rpm);
      add("speed",   liveData->speed);
      doc["age"] = (millis() - liveData->lastUpdateMs);
    }
    String out;
    serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: salva configurazione
  // ─────────────────────────────────────────────────────────
  AsyncCallbackJsonWebHandler* saveHandler =
    new AsyncCallbackJsonWebHandler("/api/config/save",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();
        CarMetrixConfig cfg;
        NVSConfig::load(cfg);
        if (obj["bleMac"].is<const char*>())
          strlcpy(cfg.bleMac, obj["bleMac"], sizeof(cfg.bleMac));
        if (obj["carBrand"].is<const char*>())
          strlcpy(cfg.carBrand, obj["carBrand"], sizeof(cfg.carBrand));
        if (obj["carProfile"].is<const char*>())
          strlcpy(cfg.carProfile, obj["carProfile"], sizeof(cfg.carProfile));
        if (obj["demoMode"].is<bool>())
          cfg.demoMode = obj["demoMode"];
        if (obj["homeSsid"].is<const char*>())
          strlcpy(cfg.homeSsid, obj["homeSsid"], sizeof(cfg.homeSsid));
        // Aggiorna la password solo se ne arriva una nuova non vuota
        if (obj["homePass"].is<const char*>()) {
          const char* p = obj["homePass"];
          if (p && p[0] != '\0') strlcpy(cfg.homePass, p, sizeof(cfg.homePass));
        }
        cfg.configured = true;
        NVSConfig::save(cfg);
        jsonReply(req, 200, "{\"ok\":true}");
        if (savedCallback) {
          delay(300);
          savedCallback();
        }
      });
  server.addHandler(saveHandler);

  // ─────────────────────────────────────────────────────────
  //  API: reset configurazione
  // ─────────────────────────────────────────────────────────
  server.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    NVSConfig::reset();
    jsonReply(req, 200, "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });

  // ─────────────────────────────────────────────────────────
  //  API: soglie alert (salva/carica da LittleFS)
  // ─────────────────────────────────────────────────────────
  server.on("/api/alerts", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists("/alerts.json"))
      req->send(LittleFS, "/alerts.json", "application/json");
    else
      req->send(200, "application/json", "[]");
  });

  AsyncCallbackJsonWebHandler* alertsHandler =
    new AsyncCallbackJsonWebHandler("/api/alerts/save",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        File f = LittleFS.open("/alerts.json", FILE_WRITE);
        if (f) { serializeJson(json, f); f.close(); }
        jsonReply(req, 200, "{\"ok\":true}");
      });
  server.addHandler(alertsHandler);

  // ─────────────────────────────────────────────────────────
  //  API: configurazione buzzer
  // ─────────────────────────────────────────────────────────
  server.on("/api/buzzer", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists("/buzzer.json")) {
      req->send(LittleFS, "/buzzer.json", "application/json");
    } else {
      const BuzzerConfig& b = AlertManager::buzzerCfg();
      JsonDocument doc;
      doc["enabled"]     = b.enabled;
      doc["warnFreq"]    = b.warnFreq;
      doc["dangerFreq"]  = b.dangerFreq;
      doc["dangerStyle"] = b.dangerStyle;
      String out; serializeJson(doc, out);
      jsonReply(req, 200, out);
    }
  });

  AsyncCallbackJsonWebHandler* buzzerHandler =
    new AsyncCallbackJsonWebHandler("/api/buzzer/save",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        File f = LittleFS.open("/buzzer.json", FILE_WRITE);
        if (f) { serializeJson(json, f); f.close(); }
        AlertManager::loadBuzzerCfg();   // applica subito, senza riavvio
        jsonReply(req, 200, "{\"ok\":true}");
      });
  server.addHandler(buzzerHandler);

  // Test tono buzzer (preview per tarare il più forte/acuto)
  AsyncCallbackJsonWebHandler* buzzerTestHandler =
    new AsyncCallbackJsonWebHandler("/api/buzzer/test",
      [](AsyncWebServerRequest* req, JsonVariant& json) {
        int freq = json["freq"] | 2700;
        AlertManager::requestTest(freq);   // il main loop suona il tono
        jsonReply(req, 200, "{\"ok\":true}");
      });
  server.addHandler(buzzerTestHandler);

  // ─────────────────────────────────────────────────────────
  //  OTA: upload firmware .bin
  // ─────────────────────────────────────────────────────────
  server.on("/api/ota/update", HTTP_POST,
    // Request handler (chiamato a upload completato)
    [](AsyncWebServerRequest* req) {
      bool ok = !Update.hasError();
      AsyncWebServerResponse* resp = req->beginResponse(
        200, "application/json",
        ok ? "{\"ok\":true,\"msg\":\"Riavvio in corso...\"}"
           : "{\"ok\":false,\"msg\":\"Aggiornamento fallito\"}"
      );
      resp->addHeader("Connection", "close");
      req->send(resp);
      if (ok) { delay(500); ESP.restart(); }
    },
    // Upload handler (chiamato per ogni chunk del file)
    [](AsyncWebServerRequest* req, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      if (!index) {
        Serial.printf("[OTA] Start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
        otaInProgress = true;
      }
      if (Update.write(data, len) != len) Update.printError(Serial);
      if (final) {
        if (Update.end(true)) Serial.printf("[OTA] OK: %u bytes\n", index + len);
        else                  Update.printError(Serial);
        otaInProgress = false;
      }
    }
  );

  // ─────────────────────────────────────────────────────────
  //  API: avvia OTA da GitHub (lato dispositivo)
  // ─────────────────────────────────────────────────────────
  server.on("/api/ota/github", HTTP_POST, [](AsyncWebServerRequest* req) {
    GithubOTA::request();   // il main loop esegue l'OTA (bloccante)
    jsonReply(req, 200, "{\"ok\":true}");
  });

  // ─────────────────────────────────────────────────────────
  //  API: stato OTA GitHub (polling dalla web UI)
  // ─────────────────────────────────────────────────────────
  server.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    const char* names[] = { "idle","connecting","checking","no_update",
                            "downloading","failed","success" };
    JsonDocument doc;
    doc["state"]    = names[(int)GithubOTA::state()];
    doc["progress"] = GithubOTA::progress();
    doc["message"]  = GithubOTA::message();
    doc["latest"]   = GithubOTA::latestVersion();
    String out; serializeJson(doc, out);
    jsonReply(req, 200, out);
  });

  // ─────────────────────────────────────────────────────────
  //  API: riavvio
  // ─────────────────────────────────────────────────────────
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    jsonReply(req, 200, "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    // Captive portal fallback
    if (req->host() != "192.168.4.1")
      req->redirect("http://192.168.4.1/");
    else
      req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[Web] Server avviato su porta 80");
}

void WebServer::stop() {
  server.end();
}

void WebServer::onConfigSaved(void (*cb)()) {
  savedCallback = cb;
}

void WebServer::setLiveData(OBDData* data) {
  liveData = data;
}
