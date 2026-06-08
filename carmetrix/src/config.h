#pragma once

// ============================================================
//  CarMetrix — config.h
//  Pin definitions, costanti globali, versione firmware
// ============================================================

#define CARMETRIX_VERSION "0.2.7"

// ── OTA da GitHub ────────────────────────────────────────────
// Il web app interroga le Release di questo repo per nuovi firmware.
// Tag release atteso: vX.Y.Z  con allegato il file carmetrix.ino.bin
#define GITHUB_OWNER  "ErPeris"
#define GITHUB_REPO   "CarMetrix"

// ── Pin hardware ─────────────────────────────────────────────
#define PIN_BUTTON   2    // bottone navigazione (INPUT_PULLUP → GND)
#define PIN_BUZZER   4    // buzzer passivo PWM
#define OLED_SDA     8
#define OLED_SCL     9

// ── WiFi portal ──────────────────────────────────────────────
#define AP_SSID_PREFIX   "CarMetrix_"   // + ultimi 4 hex del MAC
#define AP_PASSWORD      ""             // open network
#define AP_IP            "192.168.4.1"
#define DNS_PORT         53
#define WEB_PORT         80

// ── BLE / ELM327 ─────────────────────────────────────────────
// UUID di riferimento (NON usati per il match: facciamo auto-discovery).
//  - Cloni generici:        servizio FFF0, notify FFF1, write FFF2
//  - Vgate iCar Pro BLE:     servizio 18F0, notify 2AF0, write 2AF1
//  - Moduli HM-10:           servizio FFE0, char FFE1 (write+notify)
// Il firmware enumera i servizi e trova da solo NOTIFY + WRITE.
#define BLE_SERVICE_UUID  "0000FFF0-0000-1000-8000-00805F9B34FB"
#define BLE_CHAR_WRITE    "0000FFF2-0000-1000-8000-00805F9B34FB"
#define BLE_CHAR_NOTIFY   "0000FFF1-0000-1000-8000-00805F9B34FB"

#define BLE_SCAN_DURATION   8     // secondi scan iniziale
#define BLE_CONNECT_TIMEOUT 10000 // ms timeout connessione
#define BLE_RECONNECT_EVERY 3000  // ms tra tentativi riconnessione
#define ELM_CMD_TIMEOUT     2000  // ms attesa risposta AT command

// ── OBD polling ──────────────────────────────────────────────
#define OBD_POLL_INTERVAL  250    // ms tra poll (4 volte/sec)
#define OBD_DATA_TIMEOUT   5000   // ms senza dati → "NO DATA"

// ── Buzzer ───────────────────────────────────────────────────
#define BUZZER_CHANNEL    0
#define BUZZER_FREQ_WARN  1200  // Hz
#define BUZZER_FREQ_DANGER 2400 // Hz
#define BUZZER_RESOLUTION 8     // bit PWM

// ── Bottone ──────────────────────────────────────────────────
#define BTN_DEBOUNCE      200   // ms
#define BTN_LONG_PRESS    3000  // ms → entra/esce CONFIG MODE
#define BTN_FACTORY_RESET 6000  // ms → reset di fabbrica (dimentica tutto)

// ── Timeout connessione BLE ──────────────────────────────────
// Se non si connette entro questo tempo, riaccende l'hotspot
// e torna in CONFIG così l'utente riprende il controllo.
#define BLE_CONNECT_GIVEUP 60000  // ms

// ── OLED layout (bicolore 128x64) ────────────────────────────
#define OLED_YEL_BASE   9
#define OLED_DOT_Y      5
#define OLED_VALUE_Y    46
#define OLED_GAUGE_Y    50
#define OLED_GAUGE_H    5
#define OLED_GAUGE_X    4
#define OLED_GAUGE_W    120
#define OLED_SUB_Y      63
