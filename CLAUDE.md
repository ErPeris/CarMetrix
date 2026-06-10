# CLAUDE.md — CarMetrix

Guida per Claude Code su questo repository. Letto all'avvio di ogni sessione:
contiene contesto, stato attuale, gotcha e prossimi passi. **Aggiornarlo quando
lo stato del progetto cambia in modo significativo.**

## Cos'è

Display OBD2 universale su **ESP32-C3 Super Mini** + OLED SSD1306 128x64 bicolore.
Si collega via **BLE** a un adattatore ELM327 (**Vgate iCar Pro BLE 4.0**) e mostra
parametri del veicolo con allarmi configurabili (buzzer + flash). Configurazione via
**hotspot WiFi + web app captive portal**. Aggiornamenti **OTA da GitHub Releases**.

- Auto di riferimento: **Honda Civic 1.5T CVT 2017** — usa **CAN 29-bit ISO 15765-4 (ATSP7)**
- Obiettivo: funzionare su **qualsiasi auto** (PID standard Mode 01) + extra per marca (Mode 22 via profili)
- Repo: https://github.com/ErPeris/CarMetrix (git user: ErPeris / demolischer250@gmail.com — config locale)
- In arrivo: display TFT (ILI9341 2.4" touch, ST7789 1.69", GC9A01 rotondo), ESP32-S3 N16R8

## ⚠️ STATO ATTUALE (aggiornato a v0.3.2)

**Funziona:** boot, OLED, web app (embedded nel firmware), hotspot+captive portal,
mDNS carmetrix.local, scan WiFi, OTA da GitHub (testato e funzionante!), demo mode,
buzzer configurabile, **connessione BLE all'iCar Pro** (fix: scan + address type).

**NON funziona ancora: lettura dati dalla centralina.** Connessi all'ELM, le query
(es. `010F`) tornano **`NO DATA`** = l'ELM è ok ma la centralina non risponde sul bus.

**Ipotesi principale (da ricerca web):** i cloni ELM327 con CAN 29-bit spesso NON
impostano da soli l'header funzionale OBD `0x18DB33F1`. Car Scanner (che funziona
sulla stessa auto+adattatore, profilo "Honda AT/CVT CAN 29bit") presumibilmente lo fa.
La **v0.3.2** aggiunge `ATCP18` + `ATSHDB33F1` quando il protocollo è 7/9, probe
`0100` con timeout 10s, e un **ponte seriale** (vedi sotto) per la diagnosi in auto.
**La 0.3.2 non è ancora stata testata in auto.**

**Prossimo passo concreto:** sessione in auto col portatile collegato via USB.
Serial Monitor a 115200. Si possono digitare comandi AT direttamente (ponte seriale):
`!state` (stato), `!init` (rifà l'init), oppure raw: `ATSP7`, `ATSHDB33F1`, `0100`,
`010C`… e si vede la risposta grezza. Obiettivo: trovare la sequenza che fa
rispondere la ECU (motore acceso!), poi cementarla nell'init.

**Dopo il fix:** implementare il **ciclo protocollo universale** (prova ATSP0, se muto
cicla 6→7→8→9→3..5 con probe 0100, salva il protocollo che risponde in NVS) così
qualsiasi auto si aggancia senza profilo. I profili restano solo per i PID Mode 22.

## Struttura

```
CarMetrix/
├── CLAUDE.md            ← questo file
├── README.md
├── tools/embed_web.py   ← gzippa data/index.html in src/web_index.h (OBBLIGATORIO dopo modifiche UI)
└── carmetrix/
    ├── carmetrix.ino    ← state machine (CONFIG/BLE_CONNECT/MONITORING/DEMO), bottone, ponte seriale
    ├── build/           ← output compilazioni (gitignored i .bin)
    ├── src/
    │   ├── config.h         ← VERSIONE, pin, costanti (bump qui per ogni release)
    │   ├── ble_elm327.*     ← BLE client + AT commands + parser risposte (CUORE del problema attuale)
    │   ├── obd_decoder.*    ← PID Mode01 standard + Mode22 + generatore demo
    │   ├── profile_loader.* ← profili JSON da LittleFS (brand, protocol, PID estesi)
    │   ├── nvs_config.*     ← config persistente (MAC BLE, WiFi casa, profilo, demo)
    │   ├── wifi_portal.*    ← AP + DNS captive + mDNS
    │   ├── web_server.*     ← API REST + OTA upload + dati live (AsyncWebServer)
    │   ├── github_ota.*     ← OTA self-update: WiFi casa → GitHub releases/latest
    │   ├── alert_manager.*  ← soglie, buzzer PWM (config in /buzzer.json LittleFS)
    │   ├── display_oled.*   ← rendering U8g2 (in futuro display_tft.* stessa interfaccia)
    │   └── web_index.h      ← AUTO-GENERATO da embed_web.py, non editare a mano
    └── data/                ← LittleFS: SOLO profiles/*.json (la web UI è embedded)
```

## Build (Arduino IDE — l'utente compila, Claude NON ha CLI funzionante)

1. Board: **ESP32C3 Dev Module** — core esp32 v3.x (API LEDC nuove: `ledcAttach`, non `ledcSetup`)
2. ⚠️ **Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"** —
   si resetta da solo a volte! Default 1.2MB NON basta (firmware ~1.6-1.7MB).
   Budget partizione app: **1966080 byte**. Se sfora → valutare NimBLE (-400KB) o S3.
3. Se modificata la web UI: `python tools/embed_web.py` PRIMA di compilare
4. Upload normale. **LittleFS upload** (Ctrl+Shift+P → "Upload LittleFS") SOLO se
   cambiati i profili in data/ (gotcha ricorrente: profilo cambiato ma LittleFS non
   ricaricato = modifica invisibile)
5. Librerie: U8g2, **ESP Async WebServer + Async TCP (fork ESP32Async!** quello di
   lacamera è incompatibile con mbedTLS 3 e fu rimosso), ArduinoJson v7, BLE core

## Release / OTA

- Skill **`/carmetrix-release`** automatizza la pipeline (ma la compilazione CLI è rotta
  sul fisso — exit 1 non diagnosticato; per ora compila l'utente dall'IDE ed esporta).
- Processo manuale: bump `CARMETRIX_VERSION` → embed_web → compila/esporta dall'IDE →
  commit+tag `vX.Y.Z` → push → release GitHub con **carmetrix.ino.bin** allegato
  (MAI il .merged.bin). Token GitHub: già in Git Credential Manager
  (`git credential fill`). GOTCHA: /tmp di git-bash ≠ /tmp di python Windows —
  parsare le risposte curl con grep, non passando file.
- Il device controlla `releases/latest`: la versione pubblicata deve essere MAGGIORE.
- OTA del device: tab Update della web app → l'ESP si collega al WiFi di casa
  (AP+STA) e si flasha da solo. Funziona, testato con 0.2.1→0.2.3.

## Hardware attuale

| Cosa | Pin |
|---|---|
| OLED SDA / SCL | GPIO 8 / 9 |
| Pulsante → GND | GPIO 2 (INPUT_PULLUP interno; NIENTE resistenza in serie — rompe la lettura) |
| Buzzer passivo | GPIO 4 (PWM, `ledcWriteTone`) |

Pulsante: click = cambia schermata · 3s = config mode · 6s = factory reset.
Hotspot SPENTO durante BLE (C3 mono-antenna: coesistenza WiFi+BLE soffoca il BLE —
lezione della 0.2.5, non riprovarci). Timeout BLE 60s → riaccende hotspot.

## Lezioni apprese (NON ripetere questi errori)

1. **iCar Pro BLE**: servizio `18F0`, notify `2AF0`, write `2AF1`. Si chiama "iOS-Vlink".
   NIENTE pairing. Richiede connessione **via advertised device** (address type!), non
   per MAC diretto. L'auto-discovery in ble_elm327 gestisce tutto.
2. **WiFi+BLE insieme sul C3 = BLE rotto.** Mai AP acceso durante scan/connect BLE.
3. **Partizione**: se l'IDE dice "max 1310720 byte" la partizione è tornata default.
4. **LittleFS vs firmware**: modifiche a data/ richiedono upload LittleFS separato.
   (Riduzione superficie: web UI ormai embedded; valutare embed anche dei profili.)
5. **ESP32 core v3**: `ledcAttach(pin,freq,res)`, `WiFi.macAddress()`, AsyncJson.h
   per AsyncCallbackJsonWebHandler.
6. **Il telefono iOS non fa da ponte internet per l'hotspot** (no fallback dati
   cellulare) → per questo l'OTA è lato-dispositivo via WiFi di casa.
7. **Car Scanner funziona sulla stessa auto/adattatore** con profilo
   "OBD-II/EOBD + AT/CVT (CAN) Honda 29bit ISO 15765-4" → la verità di riferimento.

## Dopo lo spostamento su OneDrive

La cartella sarà spostata da `C:\Users\aless\Downloads\CarMetrix` a OneDrive per
lavorare da fisso e portatile. Da aggiornare al primo avvio nella nuova posizione:
- La skill `~/.claude/skills/carmetrix-release/SKILL.md` ha i path hardcoded → correggerli
- Verificare che git funzioni nel nuovo path (`git status`); il remote non cambia
- Sul portatile: servono Arduino IDE + librerie + plugin LittleFS (vedi Build)
- ⚠️ OneDrive può lockare i file durante la sync: se compilazioni/git danno errori
  strani, sospendere la sync mentre si lavora
