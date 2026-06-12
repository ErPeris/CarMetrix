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

## ⚠️ STATO ATTUALE (v0.3.5+db + fase A, giugno 2026, DA TESTARE)

**Fase A — quick win (12 giugno 2026, sopra v0.3.5+db, da compilare/testare):**
1. **Pulsante reattivo**: `BTN_DEBOUNCE` 200→30ms — i tap veloci (<200ms)
   venivano scartati al rilascio e il cambio schermata sembrava volere ~1s.
2. **Tab Live RIMOSSA del tutto** (web UI + endpoint `/api/live` +
   `setLiveData`): inutile, durante il MONITORING l'hotspot è spento
   (coesistenza WiFi+BLE). `web_index.h` rigenerato (gzip 10932 B).
3. **Fallback legacy `/profiles/<file>.json` rimosso** da profile_loader:
   il bundle `/profiles.json` è l'unica via (fallback interno a "generic"
   invariato).
4. Roadmap in `docs/superpowers/specs/2026-06-12-prossime-migliorie-design.md`:
   fase A=quick win (fatta), B=analisi APK Car Scanner (in corso, APK locale),
   C=migrazione NimBLE (-400KB, futura) — MAI mescolate nella stessa release.
5. `.gitignore`: `*.apk` e `log*.txt` MAI nel repo pubblico.

**Estensione "database profili" (sopra il layer v0.3.5):**
1. **Database: sorgenti in `profiles/<marca>/<modello>.json`** (root repo, git,
   leggibili) → il converter li compatta in **DUE soli file LittleFS**:
   `data/profiles.json` (bundle id→profilo, 172 profili/55 marche, 67KB) e
   `data/profiles_index.json` (18KB, `{file=id, brand, model, extras[], proto}`,
   servito da /api/profiles). ⚠️ **GOTCHA LittleFS: 1 file = minimo 1 blocco
   da 4KB** → 172 file separati ≈ 700KB, MAI mettere i profili singoli in
   data/! `profile_loader` legge il bundle col **filtro ArduinoJson** (carica
   solo l'id richiesto); il fallback legacy /profiles/<file> è stato RIMOSSO
   in fase A: senza bundle nessun profilo.
   `cfg.carProfile` ora è l'id senza .json (es. "toyota/yaris_hybrid").
2. **Converter v3** (`tools/obdb_to_profile.ps1 -All`): TUTTO l'org OBDb
   (740 repo, 101 marche), brand a due token gestiti (Mercedes-Benz,
   Land-Rover, Alfa-Romeo…), repo brand-wide → modello "Tutta la gamma",
   whitelist estesa con **hv_soc / hv_temp** (ibride/EV), 1 segnale per
   target, budget 80KB con marche prioritarie, **preserva i profili manuali**
   (campo `source` non "OBDb/..."). `-IndexOnly` rigenera solo l'indice.
3. **Web app: selezione veicolo guidata** Marca → Modello (due dropdown a
   cascata da index.json) + badge gialli dei sensori extra del profilo.
4. **Nuovi target ibridi**: `TARGET_HV_SOC`/`TARGET_HV_TEMP` → `OBDData.hvSoc/
   hvTemp`, schermate OLED `soc`/`hvtemp`, voci in /api/live e Live web.
5. **Mode 21 Toyota** supportato nel polling esteso (PID a 2 hex, risposta 61xx).
6. **`toyota/yaris_hybrid.json` scritto A MANO** (OBDb Yaris vuoto): derivato
   dalla Prius (THS condiviso) — SOC 221F5B@7D2, HV temp 2210B2@7D2, oil
   221F5C@700, ATF **mode 21** 2182@7E0. SPERIMENTALE, verificare in auto.
7. ⚠️ **NVS**: `carProfile` 48→64 byte → il blob config cambia layout; la
   load() azzera se la size non corrisponde → **setup da rifare** dopo flash
   (comunque necessario per l'upload LittleFS che cancella le config web).
8. Fonti censite: **WICAN** (meatpiHQ/wican-fw, ~40 profili per modello,
   prevalenza EV Stellantis/BYD/MG; formato con `pid_init` per-PID che include
   **ATFCSH** flow-control: da supportare nel firmware prima di convertirli —
   fase 2, mappatura documentata in testa al converter). iternio: solo EV,
   niente marche utente. Topic obd-ii-pids: librerie, non dati.

**v0.3.5 (sopra la v0.3.4, MAI flashata nemmeno quella):** layer PID proprietari
basato sul database **OBDb** (github.com/OBDb, un repo per modello).
1. **Profili schema 2** (`data/profiles/*.json`): per ogni PID `cmd` (mode+pid),
   `hdr`/`rax` (ECU target stile OBDb: 29-bit → richiesta 18<hdr>F1, risposta
   18DAF1<rax>; 11-bit → ATSH<hdr>/ATCRA<rax>), decodifica strutturata
   `bix/len/sign/mul/div/add/min/max` (bix = bit offset nel payload DOPO l'echo
   62+pid — convenzione da verificare in auto, eventualmente ±3 byte), `every`.
   Schema 1 (formula "A-40") ancora supportato. `profile_loader` espone tutto.
2. **Switch header per-PID** in pollTick: mini-coda ATSH/ATCRA inviata async
   prima delle query su ECU non attiva; item raggruppati per hdr; ripristino
   header default (fisico ECM/funzionale, registrato in `init()`) per i Mode 01.
   `ELM_RESP_MAX 64` byte (multi-frame); suffisso risposte SOLO su single-frame
   (bix+len ≤ 32 bit), altrimenti tronca le multi-frame.
3. **Converter `tools/obdb_to_profile.ps1`**: `-Repo Honda-Civic` o batch
   `-Brands BMW,Ford,...` (API GitHub org, 740 repo). Whitelist gauge:
   trans_temp/oil_temp/boost (kPa→bar relativi automatico); scarta manutenzioni,
   celle, segnali oltre 64 byte e modelli scheletro. Guardia budget 80KB.
   In testa allo script: mappatura manuale formule iternio/ev-obd-pids.
4b. **bmw_1_series.json scritto A MANO** (OBDb vuoto per la Serie 1): oil temp
   `224402` eq. B-64 (bix 8, len 8, add -64), protocollo 6, header default —
   fonte community bimmerforums.co.uk, confermato su 116i F20 2014; le F20
   ante ~2014 potrebbero non rispondere. iternio: niente BMW/Mercedes/Fiat/
   Opel/Citroën (solo Mini/Ford/Honda/VW/Renault/Jaguar/MG/GMC/HKMC/Aiways, EV).
4. **Profili generati**: honda.json = Civic da OBDb col **PID ATF vero**
   (`222201` @ TCM DA1D, byte 26, A-40 — i vecchi 1254/1756/1253 erano inventati);
   8 BMW + 13 Ford con oil/trans/boost reali. Mercedes/Fiat/Citroën su OBDb sono
   scheletri senza segnali (nessun profilo); Opel non esiste nell'org.
   Totale profiles/: ~28KB su 128KB. Nuova schermata OLED `oil` + oil in /api/live.
5. ⚠️ **Serve l'upload LittleFS** (profili cambiati) → cancella alerts/buzzer/
   screens.json salvati: config da rifare dalla web app dopo il flash.
**Da verificare in auto (Civic):** TRANS plausibile e uguale a Car Scanner
(altrimenti bix off-by: provare ±3 byte via ponte seriale); switch header nel
log (ATSH DA1DF1/ATCRA18DAF11D tra le query); Mode 01 fluidi come prima.

## Stato v0.3.4 (physical addressing + standby — incluso nella build 0.3.5)

**v0.3.3 TESTATA IN AUTO: SUCCESSO.** Log `log veloce.txt`: polling fluido,
RPM "veramente istantanei" (cit.), suffisso attivo (`n=2`), suffisso+async+skip
funzionano. Confermato: Mode 22 NO DATA (sospesi dopo 5 fail, come previsto) e
coolant/IAT fuori maschera — è il limite dell'header funzionale. A quadro spento:
raffica NO DATA e tutti i PID sospesi (gestione aggiunta in v0.3.4).

**v0.3.4 (da testare in auto):**
1. **Physical addressing ECM con fallback automatico** in `init()`: dopo il probe
   funzionale, `ATDPN` rileva il protocollo, poi prova l'header fisico
   (29-bit: ATCP18+ATSHDA10F1+ATCRA18DAF110; 11-bit: ATSH7E0+ATCRA7E8) con probe
   `01001`. Se risponde: `physicalAddressing=true`, `ecuCount=1` (suffisso "1",
   ancora più veloce), maschere lette dal .ino già in fisico → attesi 05/0F
   presenti e Mode 22 finalmente interrogabili. Se muto: ripristino funzionale
   (ATAR + DB33F1/7DF) → identico alla v0.3.3. `BleElm327::physicalOk()`.
2. **Standby motore spento**: se TUTTI i PID Mode 01 veloci (every==1) sono
   sospesi → `standby` in pollTick: solo probe `0100` ogni 10s
   (`OBD_STANDBY_PROBE_MS`); alla prima risposta azzera le sospensioni e riparte.
   Il .ino mostra "STANDBY / motore spento" (`OBDDecoder::isStandby()`).
3. **Layout OLED corretto** (foto: cifre nella zona gialla): zona gialla =
   righe 0-16, ora `OLED_VALUE_Y=49` (cifre 17-49), gauge 51-55, sub a 63.
   Sub-label SOLO in warn/danger (niente più "NOMINALE"/PSI). Fix RPM ".00".
4. **Schermate OLED dinamiche**: registry di 9 schermate standard
   (rpm/boost/map/iat/coolant/trans/speed/throttle/load) in display_oled;
   set attivo+ordine in `/screens.json` (LittleFS), default rpm,boost,trans,iat.
   API `GET /api/screens` + `POST /api/screens/save` (applica senza riavvio).
   `enum OledScreen` eliminato; pallini header = finestra di max 5 con
   freccette ◂▸ se ci sono schermate fuori vista.
5. **Conferma factory reset**: 6s → `STATE_RESET_CONFIRM` ("CONFERMA RESET?"
   con barra avanzamento); lì: 6s = reset, rilascio 3-6s = annulla, click
   breve ignorato, 15s inattività = annulla (`BTN_RESET_CONFIRM_TIMEOUT`).
6. **Web app rifatta** (mobile-first, estetica Cyberpunk 2077: nero/giallo
   acido/ciano, card con angoli tagliati, scanline): tab bar fissa in basso
   (Setup/Display/Live/Alert/Update), nuova tab **Display** per
   aggiungere/togliere/riordinare le schermate (frecce ▲▼, chip +).
   `web_index.h` rigenerato con `tools/embed_web.ps1` (gzip ~10.8KB).
   Preview locale: `tools/serve_web.ps1` (porta 8765) + `.claude/launch.json`.
   Verificata a 375px con il preview browser: tutte le tab ok.
**Da verificare in auto:** log `[ELM] Physical addressing: OK`; maschera con
05/0F (schermate COOLANT/IAT popolate); risposte singole non duplicate;
Mode 22 1254/1756/1253 rispondono? (se NO DATA anche in fisico → PID ipotizzati
sbagliati, cercarli via ponte seriale); spegni quadro → STANDBY entro ~1 min,
riaccendi → ripartenza entro ~10s; stabilità BLE.

## Stato v0.3.2 (test in auto precedenti)

**Funziona:** boot, OLED, web app (embedded nel firmware), hotspot+captive portal,
mDNS carmetrix.local, scan WiFi, OTA da GitHub (testato e funzionante!), demo mode,
buzzer configurabile, **connessione BLE all'iCar Pro** (fix: scan + address type),
e — **testato in auto: la ECU RISPONDE!** Init OK sia col profilo Honda (ATSP7 +
ATCP18/ATSHDB33F1) sia col generico (ATSP0 auto). Es: `0111` → `411124411124`.

**Note dal test in auto (giugno 2026):**
- Le risposte arrivano **duplicate** (`411124411124`): in 29-bit funzionale rispondono
  due ECU e con ATH0 le righe si concatenano. `extractBytes` lo gestisce (primo header).
- Il display restava sulla schermata diagnostica TX/RX: `pollAll` azzerava il valore
  a ogni poll fallito → **fix applicato**: tiene l'ultimo valore valido.
- **Aggiunto log seriale per ogni query** (`[OBD] 010F -> ... ok= len=`) per vedere
  in MONITORING cosa risponde ogni PID. Da ri-testare in auto.
- Col profilo Honda: `[BLE] Disconnesso` subito dopo → MONITORING (causa ignota,
  forse i 3 getSupportedPIDs consecutivi) — da osservare nei prossimi test.
- ⚠️ **USB CDC On Boot = Enabled** obbligatorio nell'IDE (si resetta da solo come il
  Partition Scheme!), altrimenti Serial va sui pin UART e il monitor USB è muto.
  arduino-cli: FQBN con `CDCOnBoot=cdc` (già aggiornato nella skill release).

**Test 2 in auto (log completo analizzato):** MAP/RPM/speed/throttle funzionano
stabilmente per minuti, identici con profilo Generic (ATSP0) e Honda (ATSP7).
**`0105` (coolant) e `010F` (IAT) → sempre NO DATA, ed è coerente**: le maschere
`0100` delle due ECU che rispondono (ECU1 `B63CA813`, ECU2 `90188003`) NON
includono 05/0F. È il comportamento Honda 10th-gen con richiesta **funzionale**
(broadcast DB33F1): risponde un set ridotto. Per coolant/IAT e per i Mode 22
serve l'**indirizzamento fisico alla ECM**: header richiesta `18DA10F1`
(ATSHDA10F1), risposta attesa `18DAF110`. Car Scanner presumibilmente fa così.
Anche i 3 Mode 22 Honda (221254/221756/221253) → NO DATA per lo stesso motivo
(UDS richiede physical addressing), oltre a essere PID ipotizzati.
Il "bloccato in diagnostica" era solo la schermata default IAT senza dati.
**Fix applicati dopo il test 2:** pollAll salta i PID assenti dalla maschera 0100;
schermata iniziale scelta in base alla maschera (Civic → parte da BOOST).

**Prossimo passo concreto (test 3, ponte seriale in auto):** verificare il
physical addressing: `ATSHDA10F1` poi `0100`, `0105`, `010F` (attesa: maschera
completa e risposte singole `41...`), poi `221254`/`221756`/`221253` per i Mode 22.
Se funziona: init con doppio header (funzionale per probe, fisico per polling)
o direttamente fisico per tutto. Poi confermare i PID Mode 22 Honda veri.

**Dopo:** implementare il **ciclo protocollo universale** (prova ATSP0, se muto
cicla 6→7→8→9→3..5 con probe 0100, salva il protocollo che risponde in NVS) così
qualsiasi auto si aggancia senza profilo. I profili restano solo per i PID Mode 22.
Nota: ATSP0 ha già agganciato la Civic al primo colpo → l'auto-detect è promettente.

**Idee da VaAndCob/ESP32-Bluetooth-OBD2-Gauge** (analizzato giugno 2026; il repo
"Serial" omonimo è vuoto, i sorgenti sono nel predecessore Bluetooth):
- **Polling non bloccante**: nel loop leggono i char in arrivo e al prompt `>` mandano
  subito il PID successivo. Niente busy-wait → display sempre fluido. Il nostro
  `pollAll` invece blocca il loop per l'intero giro (fino a 5s/query in timeout).
  Refactor candidato quando il flusso dati è confermato stabile.
- **Skip-rate per PID**: temperature (coolant/IAT/trans) ogni 3-4 cicli, RPM/MAP/load
  a ogni ciclo → più banda ai valori veloci.
- **ATAT2** (adaptive timing aggressivo) al posto di ATAT1 — cambio di una riga, da provare.
- **Deep sleep a motore spento**: RPM è l'ultimo PID del giro; se 0/assente → deep
  sleep 3 min e retry. Utile: molte prese OBD restano alimentate a quadro spento.
- Il loro parser è più fragile del nostro (split su spazi, singola ECU) → non copiare.

## Struttura

```
CarMetrix/
├── CLAUDE.md            ← questo file
├── README.md
├── web/index.html       ← SORGENTE web UI (NON in data/: embedded nel firmware)
├── profiles/            ← SORGENTI database profili (git; <marca>/<modello>.json)
├── tools/embed_web.ps1  ← gzippa web/index.html in src/web_index.h (OBBLIGATORIO dopo modifiche UI)
├── tools/obdb_to_profile.ps1 ← converter OBDb→profili + bundle device (-All / -Repo / -IndexOnly)
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
    └── data/                ← LittleFS: SOLO profiles.json (bundle) + profiles_index.json
                               (web UI embedded; MAI file piccoli multipli: 1 file = 4KB!)
```

## Build

**arduino-cli FUNZIONA** (installata giugno 2026 in `C:\Users\aless\arduino-cli\`;
NON è nel PATH delle shell non interattive → usare sempre il percorso completo;
core esp32 3.3.10 condiviso con l'IDE in AppData\Local\Arduino15;
librerie in `C:\Users\aless\Documents\Arduino\libraries` — locale, fuori OneDrive):
```powershell
& "C:\Users\aless\arduino-cli\arduino-cli.exe" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" --build-path "$env:LOCALAPPDATA\arduino\carmetrix-cli-build" --output-dir carmetrix\build carmetrix
```
⚠️ **SEMPRE con `--build-path` dedicato**: senza, arduino-cli usa la stessa cache
dell'IDE (`AppData\Local\arduino\sketches\<hash>`) e una build CLI in parallelo a
una build IDE dell'utente le corrompe entrambe (successo giugno 2026: linker
"cannot find carmetrix.ino.cpp.o"). Se la cache IDE risulta corrotta: cancellare
la cartella `sketches\<hash>` e ricompilare.
Verificata su v0.3.3: 1.669.321 byte (84% di 1966080). Niente Python su questa
macchina: per la web UI usare `tools/embed_web.ps1` (equivalente PowerShell di
embed_web.py). L'utente può comunque compilare/flashare dall'IDE:

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

- Skill **`/carmetrix-release`** (ricreata giugno 2026 in `~/.claude/skills/`, path
  aggiornati post-OneDrive) automatizza tutta la pipeline: bump `CARMETRIX_VERSION` →
  embed_web (se UI cambiata) → compile arduino-cli → commit+tag `vX.Y.Z` → push →
  release GitHub via API REST con **carmetrix.ino.bin** allegato (MAI il .merged.bin).
  Token GitHub: in Git Credential Manager (`git credential fill`), niente `gh` su
  questa macchina. API chiamate da PowerShell con Invoke-RestMethod (il vecchio
  gotcha /tmp git-bash vs python non si applica più).
- Il device controlla `releases/latest`: la versione pubblicata deve essere MAGGIORE.
- OTA del device: tab Update della web app → l'ESP si collega al WiFi di casa
  (AP+STA) e si flasha da solo. Funziona, testato con 0.2.1→0.2.3.

## Hardware attuale

| Cosa | Pin |
|---|---|
| OLED SDA / SCL | GPIO 8 / 9 |
| Pulsante → GND | GPIO 2 (INPUT_PULLUP interno; NIENTE resistenza in serie — rompe la lettura) |
| Buzzer passivo | GPIO 3 (PWM, `ledcWriteTone`; era GPIO 4) |

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
