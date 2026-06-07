#include "wifi_portal.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>

static DNSServer dnsServer;
static String    apSSID;
static bool      running = false;

void WiFiPortal::begin() {
  // SSID univoco basato su MAC (ultimi 4 hex)
  // WiFi.macAddress() è disponibile senza include aggiuntivi
  String macStr = WiFi.macAddress();          // "AA:BB:CC:DD:EE:FF"
  String suffix = macStr.substring(12, 14) +
                  macStr.substring(15, 17);   // "EEFF"
  suffix.replace(":", "");
  apSSID = String(AP_SSID_PREFIX) + suffix;   // es. "CarMetrix_EEFF"

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  WiFi.softAP(apSSID.c_str(), AP_PASSWORD[0] ? AP_PASSWORD : nullptr);

  // DNS catch-all → qualsiasi dominio risponde con 192.168.4.1
  // iOS e Android rilevano la mancanza di internet e aprono il browser
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));

  running = true;
  Serial.printf("[Portal] AP: %s  IP: %s\n", apSSID.c_str(), AP_IP);
}

void WiFiPortal::loop() {
  if (running) dnsServer.processNextRequest();
}

void WiFiPortal::stop() {
  if (!running) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  running = false;
}

String WiFiPortal::getSSID() { return apSSID; }
String WiFiPortal::getIP()   { return AP_IP; }
