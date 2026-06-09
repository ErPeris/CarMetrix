#include "profile_loader.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static String                        loadedBrand;
static String                        loadedProtocol = "0";   // ATSP, "0" = auto
static std::vector<ProfileLoader::ExtendedPID> extPIDs;
static bool                          loaded = false;

// Mappa stringa target → enum
static ProfileLoader::PidTarget parseTarget(const char* s) {
  String t = s;
  t.toLowerCase();
  if (t == "boost")      return ProfileLoader::TARGET_BOOST;
  if (t == "trans_temp") return ProfileLoader::TARGET_TRANS_TEMP;
  if (t == "oil_temp")   return ProfileLoader::TARGET_OIL_TEMP;
  if (t == "turbo_rpm")  return ProfileLoader::TARGET_TURBO_RPM;
  return ProfileLoader::TARGET_CUSTOM;
}

bool ProfileLoader::load(const char* profileFile) {
  extPIDs.clear();
  loaded = false;

  // Costruisci path completo
  String path = "/profiles/";
  path += profileFile;

  if (!LittleFS.exists(path)) {
    Serial.printf("[Profile] File non trovato: %s\n", path.c_str());
    // Carica generic come fallback
    if (String(profileFile) != "generic.json")
      return load("generic.json");
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[Profile] JSON error: %s\n", err.c_str());
    return false;
  }

  loadedBrand    = doc["brand"]    | "Generic";
  loadedProtocol = doc["protocol"] | "0";   // es. "6" = CAN 11bit 500k

  // Carica PID estesi (Mode 22), presenti solo nei profili produttore
  JsonArray pids = doc["pids"].as<JsonArray>();
  for (JsonObject p : pids) {
    ExtendedPID ep;
    strlcpy(ep.name,    p["name"]    | "unknown", sizeof(ep.name));
    strlcpy(ep.unit,    p["unit"]    | "",         sizeof(ep.unit));
    strlcpy(ep.formula, p["formula"] | "A",        sizeof(ep.formula));

    // PID in hex stringa "1754" → uint16_t
    const char* pidStr = p["id"] | "0000";
    ep.pidHex = (uint16_t)strtol(pidStr, nullptr, 16);
    ep.target = parseTarget(p["target"] | "custom");

    extPIDs.push_back(ep);
  }

  loaded = true;
  Serial.printf("[Profile] Caricato: %s (%d PID estesi)\n",
                loadedBrand.c_str(), (int)extPIDs.size());
  return true;
}

void ProfileLoader::unload() {
  extPIDs.clear();
  loadedBrand = "";
  loaded = false;
}

const char* ProfileLoader::getBrand() {
  return loadedBrand.c_str();
}

const char* ProfileLoader::getProtocol() {
  return loadedProtocol.c_str();
}

const std::vector<ProfileLoader::ExtendedPID>& ProfileLoader::getExtendedPIDs() {
  return extPIDs;
}

bool ProfileLoader::hasExtended() {
  return loaded && !extPIDs.empty();
}
