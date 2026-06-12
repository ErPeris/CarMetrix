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
  if (t == "hv_soc")     return ProfileLoader::TARGET_HV_SOC;
  if (t == "hv_temp")    return ProfileLoader::TARGET_HV_TEMP;
  return ProfileLoader::TARGET_CUSTOM;
}

static void parsePids(JsonVariantConst doc, int schema);

// profileFile = id nel bundle ("toyota/yaris_hybrid"); accetta anche il
// vecchio formato con estensione ("honda.json") per retro-compatibilità.
bool ProfileLoader::load(const char* profileFile) {
  extPIDs.clear();
  loaded = false;

  String id = profileFile;
  id.replace(".json", "");
  if (id.length() == 0) id = "generic";

  // ── Bundle unico /profiles.json (id → profilo) ────────────
  // Un solo file su LittleFS (blocchi da 4KB: 170+ file separati non
  // entrerebbero mai nei 128KB). Il filtro ArduinoJson estrae SOLO la
  // chiave richiesta: niente parsing degli altri ~170 profili in RAM.
  if (LittleFS.exists("/profiles.json")) {
    File f = LittleFS.open("/profiles.json", "r");
    if (!f) return false;
    JsonDocument filter;
    filter[id] = true;
    JsonDocument doc;
    DeserializationError err =
      deserializeJson(doc, f, DeserializationOption::Filter(filter));
    f.close();

    JsonVariantConst p = doc[id];
    if (err || p.isNull()) {
      Serial.printf("[Profile] '%s' non nel bundle (%s)\n",
                    id.c_str(), err ? err.c_str() : "assente");
      if (id != "generic") return load("generic");
      return false;
    }
    loadedBrand    = id;
    loadedProtocol = p["protocol"] | "0";
    parsePids(p, p["schema"] | 1);
    loaded = true;
    Serial.printf("[Profile] Caricato dal bundle: %s (%d PID estesi)\n",
                  id.c_str(), (int)extPIDs.size());
    return true;
  }

  Serial.println("[Profile] /profiles.json mancante su LittleFS — nessun profilo caricato");
  return false;
}

// Carica i PID estesi (Mode 21/22) da un oggetto profilo (bundle o file)
static void parsePids(JsonVariantConst doc, int schema) {
  JsonArrayConst pids = doc["pids"].as<JsonArrayConst>();
  for (JsonObjectConst p : pids) {
    ProfileLoader::ExtendedPID ep = {};
    strlcpy(ep.name, p["name"] | "unknown", sizeof(ep.name));
    strlcpy(ep.unit, p["unit"] | "",        sizeof(ep.unit));
    ep.target = parseTarget(p["target"] | "custom");
    ep.mode   = 0x22;
    ep.every  = p["every"] | 4;
    ep.mul = 1; ep.div = 1; ep.add = 0;
    ep.mn = -1e9f; ep.mx = 1e9f;

    if (schema >= 2) {
      // Schema 2 (da OBDb): "cmd" = mode+pid, decodifica strutturata
      const char* cmd = p["cmd"] | "220000";
      char modeStr[3] = { cmd[0], cmd[1], 0 };
      ep.mode   = (uint8_t)strtol(modeStr, nullptr, 16);
      ep.pidHex = (uint16_t)strtol(cmd + 2, nullptr, 16);
      strlcpy(ep.hdr, p["hdr"] | "", sizeof(ep.hdr));
      strlcpy(ep.rax, p["rax"] | "", sizeof(ep.rax));
      ep.structured = true;
      ep.bix  = p["bix"]  | 0;
      ep.len  = p["len"]  | 8;
      ep.sign = p["sign"] | false;
      ep.mul  = p["mul"]  | 1.0f;
      ep.div  = p["div"]  | 1.0f;
      ep.add  = p["add"]  | 0.0f;
      ep.mn   = p["min"]  | -1e9f;
      ep.mx   = p["max"]  |  1e9f;
    } else {
      // Schema 1 legacy: "id" + "formula"
      strlcpy(ep.formula, p["formula"] | "A", sizeof(ep.formula));
      const char* pidStr = p["id"] | "0000";
      ep.pidHex = (uint16_t)strtol(pidStr, nullptr, 16);
      ep.structured = false;
    }

    if (ep.div == 0) ep.div = 1;   // mai dividere per zero
    extPIDs.push_back(ep);
  }
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
