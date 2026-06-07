#include "github_ota.h"
#include "config.h"
#include "nvs_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

static volatile bool requested = false;
static GithubOTA::State st = GithubOTA::IDLE;
static int     prog   = 0;
static String  msg    = "";
static String  latest = "";

void              GithubOTA::request()        { requested = true; }
bool              GithubOTA::isRequested()    { return requested; }
GithubOTA::State  GithubOTA::state()          { return st; }
int               GithubOTA::progress()       { return prog; }
const char*       GithubOTA::message()        { return msg.c_str(); }
const char*       GithubOTA::latestVersion()  { return latest.c_str(); }

// Confronto semver: 1 se a>b, -1 se a<b, 0 se uguali
static int cmpVer(const String& a, const String& b) {
  int a1=0,a2=0,a3=0,b1=0,b2=0,b3=0;
  sscanf(a.c_str(), "%d.%d.%d", &a1, &a2, &a3);
  sscanf(b.c_str(), "%d.%d.%d", &b1, &b2, &b3);
  if (a1 != b1) return a1 > b1 ? 1 : -1;
  if (a2 != b2) return a2 > b2 ? 1 : -1;
  if (a3 != b3) return a3 > b3 ? 1 : -1;
  return 0;
}

void GithubOTA::run() {
  requested = false;
  prog = 0;
  latest = "";

  CarMetrixConfig cfg;
  NVSConfig::load(cfg);
  if (cfg.homeSsid[0] == '\0') {
    st = FAILED; msg = "WiFi di casa non configurato";
    return;
  }

  // ── 1. Connessione STA mantenendo l'AP attivo ──────────────
  st = CONNECTING; msg = "Connessione al WiFi di casa...";
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.homeSsid, cfg.homePass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    st = FAILED; msg = "WiFi di casa non raggiungibile";
    WiFi.disconnect(false);   // chiude STA, lascia l'AP
    return;
  }

  // ── 2. Query release GitHub (TLS in blocco così si libera) ─
  String assetUrl = "";
  {
    st = CHECKING; msg = "Controllo release su GitHub...";
    WiFiClientSecure client; client.setInsecure();   // no validazione cert
    HTTPClient https;
    String api = String("https://api.github.com/repos/")
                 + GITHUB_OWNER + "/" + GITHUB_REPO + "/releases/latest";
    https.begin(client, api);
    https.addHeader("User-Agent", "CarMetrix-ESP32");  // GitHub lo richiede
    https.setTimeout(10000);
    int code = https.GET();
    if (code != 200) {
      st = FAILED; msg = "GitHub HTTP " + String(code);
      https.end(); WiFi.disconnect(false);
      return;
    }
    String payload = https.getString();
    https.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      st = FAILED; msg = "Risposta GitHub non valida";
      WiFi.disconnect(false); return;
    }
    String tag = doc["tag_name"] | "";
    tag.replace("v", "");
    latest = tag;
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
      String name = a["name"] | "";
      if (name.endsWith(".bin")) { assetUrl = a["browser_download_url"] | ""; break; }
    }
  }

  if (cmpVer(latest, CARMETRIX_VERSION) <= 0) {
    st = NO_UPDATE; msg = "Già aggiornato (v" CARMETRIX_VERSION ")";
    WiFi.disconnect(false);
    return;
  }
  if (assetUrl.isEmpty()) {
    st = FAILED; msg = "Nessun file .bin nella release";
    WiFi.disconnect(false); return;
  }

  // ── 3. Download + flash (httpUpdate segue il redirect del CDN) ─
  st = DOWNLOADING; msg = "Scarico v" + latest + "...";
  Update.onProgress([](size_t done, size_t total) {
    prog = total ? (int)(done * 100 / total) : 0;
  });

  WiFiClientSecure upClient; upClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);   // riavvia da solo a fine flash

  t_httpUpdate_return ret = httpUpdate.update(upClient, assetUrl);
  // Se arriviamo qui non ha riavviato → esito negativo
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      st = FAILED; msg = "Flash fallito: " + httpUpdate.getLastErrorString(); break;
    case HTTP_UPDATE_NO_UPDATES:
      st = NO_UPDATE; msg = "Nessun aggiornamento"; break;
    default:
      st = SUCCESS; msg = "Completato"; break;
  }
  WiFi.disconnect(false);
}
