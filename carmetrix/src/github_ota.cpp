#include "github_ota.h"
#include "config.h"
#include "nvs_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

// Token GitHub per OTA da repo privata (secrets.h, gitignored). Assente o
// "" = repo pubblica → l'OTA funziona senza autenticazione, come prima.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef GITHUB_TOKEN
  #define GITHUB_TOKEN ""
#endif

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

  // NVS prioritario (impostato dalla web app), fallback al macro di secrets.h
  const char* token = cfg.githubToken[0] ? cfg.githubToken : GITHUB_TOKEN;
  const bool  hasToken = (token[0] != '\0');

  // ── 2. Query release GitHub (TLS in blocco così si libera) ─
  // assetDownUrl = browser_download_url (repo pubblica, scaricabile diretto)
  // assetApiUrl  = .../releases/assets/<id> (repo privata, serve token + risolvere il redirect)
  String assetDownUrl = "", assetApiUrl = "";
  {
    st = CHECKING; msg = "Controllo release su GitHub...";
    WiFiClientSecure client; client.setInsecure();   // no validazione cert
    HTTPClient https;
    String api = String("https://api.github.com/repos/")
                 + GITHUB_OWNER + "/" + GITHUB_REPO + "/releases/latest";
    https.begin(client, api);
    https.addHeader("User-Agent", "CarMetrix-ESP32");  // GitHub lo richiede
    if (hasToken) https.addHeader("Authorization", String("Bearer ") + token);
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
      if (name.endsWith(".bin")) {
        assetDownUrl = a["browser_download_url"] | "";
        assetApiUrl  = a["url"] | "";
        break;
      }
    }
  }

  if (cmpVer(latest, CARMETRIX_VERSION) <= 0) {
    st = NO_UPDATE; msg = "Già aggiornato (v" CARMETRIX_VERSION ")";
    WiFi.disconnect(false);
    return;
  }
  if ((hasToken ? assetApiUrl : assetDownUrl).isEmpty()) {
    st = FAILED; msg = "Nessun file .bin nella release";
    WiFi.disconnect(false); return;
  }

  // ── 3. Risoluzione URL di download ─────────────────────────
  // Repo pubblica: browser_download_url è scaricabile diretto (httpUpdate
  // segue il redirect del CDN). Repo privata: l'asset API redirige a un URL
  // S3 *firmato* che NON accetta l'header Authorization → va risolto a mano
  // (no follow), poi si scarica l'URL S3 senza token.
  String downloadUrl = assetDownUrl;
  if (hasToken) {
    WiFiClientSecure rc; rc.setInsecure();
    HTTPClient h;
    h.begin(rc, assetApiUrl);
    h.addHeader("User-Agent", "CarMetrix-ESP32");
    h.addHeader("Authorization", String("Bearer ") + token);
    h.addHeader("Accept", "application/octet-stream");
    const char* collect[] = { "Location" };
    h.collectHeaders(collect, 1);
    h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h.setTimeout(10000);
    int rcode = h.GET();
    downloadUrl = h.header("Location");
    h.end();
    if (downloadUrl.isEmpty()) {
      st = FAILED; msg = "Asset privato: redirect mancante (HTTP " + String(rcode) + ")";
      WiFi.disconnect(false); return;
    }
  }

  // ── 4. Download + flash (URL S3 firmato o CDN: nessun token) ─
  st = DOWNLOADING; msg = "Scarico v" + latest + "...";
  Update.onProgress([](size_t done, size_t total) {
    prog = total ? (int)(done * 100 / total) : 0;
  });

  WiFiClientSecure upClient; upClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);   // riavvia da solo a fine flash

  t_httpUpdate_return ret = httpUpdate.update(upClient, downloadUrl);
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
