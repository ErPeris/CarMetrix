# CarMetrix

Display OBD2 universale per auto, basato su ESP32. Si collega a un adattatore
OBD2 Bluetooth (ELM327, es. Vgate iCar Pro) e mostra in tempo reale i parametri
del veicolo su un display, con allarmi sonori/visivi configurabili.

## Caratteristiche

- 📡 **BLE** verso adattatori ELM327 (auto-riconnessione)
- 🚗 **Profili veicolo** per PID proprietari (Mode 22) — Honda, VW/Audi, BMW, Toyota, Ford + Generic
- 📊 **Display** con gauge, navigazione a pulsante, schermate IAT / Boost / Trans / RPM
- 🔔 **Allarmi** con soglie configurabili (buzzer + flash schermo)
- 🌐 **Web UI** via hotspot WiFi (captive portal) per setup, dashboard live, soglie
- 🧪 **Demo Mode** — dati simulati per testare tutto senza OBD collegato
- ⬆️ **OTA** — aggiornamento firmware dal browser (upload manuale o da GitHub Release)

## Hardware

| Componente | Note |
|---|---|
| ESP32-C3 Super Mini / ESP32-S3 | MCU principale |
| Display OLED SSD1306 128×64 | (TFT ILI9341 / ST7789 / GC9A01 in arrivo) |
| Adattatore OBD2 BLE (ELM327) | es. Vgate iCar Pro |
| Pulsante | navigazione schermate |
| Buzzer passivo | feedback sonoro allarmi |

### Cablaggio (ESP32-C3 Super Mini)

| Segnale | GPIO |
|---|---|
| OLED SDA | 8 |
| OLED SCL | 9 |
| Pulsante → GND | 2 |
| Buzzer → GND | 4 |

## Build & flash

1. Arduino IDE, board **ESP32C3 Dev Module**
2. Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**
3. Librerie: `U8g2`, `ESPAsyncWebServer` + `AsyncTCP` (ESP32Async), `ArduinoJson`
4. Se hai modificato la web UI: `python tools/embed_web.py` (rigenera `src/web_index.h`)
5. Compila e carica lo sketch (`carmetrix/carmetrix.ino`)
6. (Solo per i profili veicolo) carica il filesystem: `Ctrl+Shift+P` → *Upload LittleFS*

> La web UI (`index.html`) è **embedded nel firmware** (gzip) → si aggiorna via OTA.
> LittleFS serve solo per i profili veicolo e per la config runtime (alert/buzzer).

## Primo avvio

1. Connettiti all'hotspot WiFi `CarMetrix_XXXX`
2. Il browser apre il portale di setup
3. Seleziona adattatore BLE + profilo veicolo → Salva
4. Da quel momento la riconnessione è automatica

## Versionamento

Il numero di versione è in [`carmetrix/src/config.h`](carmetrix/src/config.h)
(`CARMETRIX_VERSION`). Ogni release GitHub usa un tag `vX.Y.Z` corrispondente,
con il firmware `carmetrix.ino.bin` allegato per l'OTA.

## Licenza

MIT
