#include "nvs_config.h"
#include <Preferences.h>

static Preferences prefs;
static const char* NS = "carmetrix";

void NVSConfig::load(CarMetrixConfig& cfg) {
  memset(&cfg, 0, sizeof(cfg));   // default sicuri (anche per nuovi campi)
  prefs.begin(NS, true);
  size_t n = prefs.getBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  // Blob assente O di dimensione diversa (layout struct cambiato tra
  // versioni, es. carProfile 48→64): i campi sarebbero disallineati →
  // riparti da config vuota (setup da rifare, niente dati corrotti).
  if (n != sizeof(cfg)) memset(&cfg, 0, sizeof(cfg));
}

void NVSConfig::save(const CarMetrixConfig& cfg) {
  prefs.begin(NS, false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}

void NVSConfig::reset() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
}

bool NVSConfig::isConfigured() {
  CarMetrixConfig cfg;
  load(cfg);
  return cfg.configured && cfg.bleMac[0] != '\0';
}

void NVSConfig::setBleMac(const char* mac) {
  CarMetrixConfig cfg;
  load(cfg);
  strncpy(cfg.bleMac, mac, sizeof(cfg.bleMac) - 1);
  save(cfg);
}

void NVSConfig::setCarProfile(const char* brand, const char* profileFile) {
  CarMetrixConfig cfg;
  load(cfg);
  strncpy(cfg.carBrand,   brand,       sizeof(cfg.carBrand) - 1);
  strncpy(cfg.carProfile, profileFile, sizeof(cfg.carProfile) - 1);
  save(cfg);
}

void NVSConfig::setObdProtocol(uint8_t proto) {
  CarMetrixConfig cfg;
  load(cfg);
  cfg.obdProtocol = proto;
  save(cfg);
}

void NVSConfig::setPidMasks(uint32_t m1, uint32_t m2, uint32_t m3) {
  CarMetrixConfig cfg;
  load(cfg);
  cfg.pidMask1 = m1;
  cfg.pidMask2 = m2;
  cfg.pidMask3 = m3;
  cfg.configured = true;
  save(cfg);
}
