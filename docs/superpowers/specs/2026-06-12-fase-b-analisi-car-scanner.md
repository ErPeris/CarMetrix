# Fase B — Analisi APK Car Scanner (com.ovz.carscanner, ELM OBD2 2.0.30)

Analisi statica dell'APK legittimo (59.6 MB) fornito dall'utente, copia che
possiede sul telefono. Scopo: interoperabilità — capire l'architettura di
connessione e l'organizzazione dei dati per rendere CarMetrix competitivo,
NON copiare il database proprietario. Materiale tenuto solo in locale
(`.gitignore`: `*.apk`).

## Metodo

Car Scanner è un'app **.NET MAUI** (non Java/Kotlin): il codice e i dati stanno
negli **assembly .NET**, non nei classes.dex (che contengono solo il bootstrap
Android + Aptoide SDK). Gli assembly sono in
`lib/<abi>/libassemblies.<abi>.blob.so` (39 MB), uno **store .NET** con ogni
DLL compressa **LZ4** (header `XALZ`). Procedura:
1. Estratto il blob, scansionati i 183 blocchi `XALZ`, decompressi con un
   decoder LZ4-block scritto ad-hoc → 57 MB di assembly in chiaro.
2. Ricerca di stringhe: comandi AT, URL, nomi `.db`, proprietà dei modelli C#
   (`get_*`/`set_*`), header del magic SQLite.

## Conclusione che cambia il piano: i profili NON sono nell'APK

**Risultato netto e verificato:**
- **0 file `.db`/SQLite** dentro l'APK (solo font negli `assets/`).
- **0 header `SQLite format 3`** nei 57 MB di assembly decompressi.
- **Unico host Car Scanner**: `https://www.carscanner.info`.

→ **Tutti i database veicolo (PID/sensori live E coding) sono scaricati a
runtime dal server di Car Scanner**, per-veicolo. L'APK contiene solo il
**motore** (modelli dati, parser formule, logica AT) e un **indice di
fingerprint ECU** incorporato come stringa.

**Implicazione sul design originario** ("fare un dump e copiarne i profili"):
non è possibile dall'APK. La verifica mirata (goal 3) si farà **solo via ponte
seriale** (già previsto come "test 3" nel CLAUDE.md) o cattura di rete — non
per estrazione statica.

## 1. Modello dei "connection profile" (la gamba che ci serve di più)

Car Scanner NON hardcoda sequenze AT: le **costruisce a runtime** dai campi di
un modello dati ricco. Proprietà estratte dagli assembly (nomi C# reali):

**Comandi e fasi**
- `BeforeCommand`/`BeforeCommands`, `AfterCommand`/`AfterCommands`,
  `AllTimeCommand`, `AcceptCommand`, `StartCommand`, `RebootCommand` →
  sequenze inviate prima/dopo le query e all'aggancio ECU. È esattamente lo
  **switch header per-PID** che CarMetrix fa a mano: qui è dato-guidato.
- `DetectECUConnectionPID`, `DependencyPID` → PID-sonda per riconoscere che
  una ECU risponde, e dipendenze tra PID.

**Header / addressing**
- `ATCRA` + `ATCRAOptimization`, `ATCEA`, `RequestHeader`, `ResponseHeader`.
  Conferma il nostro approccio (ATSH/ATCRA), con un'**ottimizzazione**: evita
  di re-inviare ATCRA se non cambia (`ATCommandStateOptimization`).

**Timing**
- `AdaptiveTimings`/`AdaptiveTimingsList`, `ATST` (timeout, es. `"ATST":"64"`).
  → adaptive timing per-profilo, non globale.

**Throughput CAN (multi-PID e multi-frame)**
- `CANOptimizeRequests`, `CANOptimizeMode`, `CANOptimizeMaxInRequest` →
  **più PID in una singola richiesta** (il nostro raggruppamento per hdr, ma
  spinto: max PID per richiesta configurabile).
- `CANRequestSegmentationSTNLevel`, `CANResponseSegmentationSTNLevel` →
  segmentazione ISO-TP via chip **STN** (oltre l'ELM327 classico).
- `FlowControlMode`, `FlowControlOverrideMode` → gestione **flow control**
  (ATFCSH/ATFCSD/ATFCSM) per le risposte multi-frame. È la fase 2 WICAN già
  annotata nel converter: qui è confermata come necessaria per le ECU che
  rispondono in multi-frame.
- `ATCommandsNotSupported` → lista di AT che un dato adattatore non supporta
  (gestione quirk per-adattatore).

## 2. Organizzazione del database

**Per-marca + per-ECU.** Nomi `.db` estratti (riferiti, non presenti):
`brands.db`, `BMW.db`, `AlfaRomeo.db`, `Chrysler.db`, `Changan.db`, …
e per-ECU come `EMS3140_29_1003.db`, `EMS3141_11.db`, `ABSESC_X_ALL_10C0.db`,
`EDC17C84_11_10C0.db`. **Il nome del file codifica il connection profile:**
- ECU/centralina (es. `EMS3140`, `EDC17C84`, `ABSESC`),
- **larghezza CAN**: `_11_` (11-bit) / `_29_` (29-bit),
- **sessione UDS/StartCommand**: `_1003` (extended diagnostic session) /
  `_10C0` (sessione custom).

**Fingerprint ECU.** L'indice incorporato ha voci tipo:
```json
{"Filename":"ABSESC_X_ALL_1003.db",
 "Idents":[{"RequestHeader":"740","diagversion":4,"supplier":"BOSCH",
            "version":"N60CG000077 - MLC70/2","soft":"BSVDC"}, …],
 "RequestHeader":"740","ResponseHeader":"760","ATST":"64",
 "StartCommand":"10C0","RebootCommand":""}
```
→ Car Scanner interroga la ECU (header 740→760), legge supplier/versione
software, e **sceglie il database giusto per quella variante esatta** di
centralina. È un livello di precisione (variante hardware/software) che
CarMetrix non ha: noi mappiamo per marca/modello, loro per **firmware ECU**.

**Motore formule.** `BuildCANFormula`, `CustomizableFormulaDB`, `ABFormula`,
PID personalizzati editabili (`EditableCustomPID`, "insert PID in formula").
Le formule sono **tokenizzate nei `.db`** (nessuna espressione `A*256+B` in
chiaro negli assembly) → il nostro schema 2 (`bix/len/mul/div/add`) è più
semplice ma copre i casi gauge; loro hanno un mini-linguaggio di formule.

**Namespace ECUModels** con sottomodelli `.Base/.OBD/.Response/.VAG` →
gerarchia: modello base OBD standard + override per gruppo (VAG, ecc.).

## 3. Verifica mirata (Civic, yaris_hybrid, bmw_1_series)

**Non eseguibile dall'APK** (database non presenti). Va fatta col **ponte
seriale** già in CarMetrix:
- Opzione A (consigliata): collegare Car Scanner alla Civic, e in parallelo/
  successione usare il ponte seriale di CarMetrix per inviare gli stessi
  comandi (`ATSHDA10F1`/`0100`/`0105`/`010F`, poi i Mode 22) e confrontare le
  risposte grezze. Verità di riferimento citata nel CLAUDE.md.
- Opzione B: cattura di rete (mitmproxy sul telefono) mentre Car Scanner
  scarica il `.db` della Civic → si ottiene il file reale per quel veicolo.
  Più invasiva e potenzialmente contraria ai ToS: da valutare, non default.

## 4. Backlog derivato per CarMetrix (priorità)

Idee dato-guidate, ordinate per rapporto valore/sforzo:

1. **Profilo connessione dato-guidato** (alta): generalizzare lo switch header
   da codice a dati nel profilo — campi `beforeCommands`/`afterCommands` per
   ECU, sul modello Car Scanner. Abilita il ciclo protocollo universale e il
   Mode 22 senza patch C++ per ogni auto.
2. **`detectPID` per ECU** (alta): un PID-sonda per profilo per confermare che
   la ECU risponde prima di interrogarla (riduce i NO DATA, già visti sulla
   Civic con header funzionale).
3. **Flow control ATFCSH** (media): prerequisito per i profili WICAN e per le
   ECU multi-frame. Implementare `ATFCSH/ATFCSD/ATFCSM` nel firmware (era già
   "fase 2" nel converter).
4. **CAN optimize: più PID per richiesta** (media): spingere il raggruppamento
   attuale verso N PID in una query, con `maxInRequest` configurabile.
5. **`atCommandsNotSupported` per adattatore** (bassa): gestione quirk
   dell'iCar Pro / cloni, per robustezza.
6. **Fingerprint ECU** (bassa/futura): troppo oltre per ora, ma è IL motivo
   per cui Car Scanner "becca" varianti che i profili per-modello mancano.

**Esperienza d'uso (il vantaggio competitivo richiesto).** Car Scanner ha
fingerprinting e download a runtime → richiede rete e setup. CarMetrix può
essere *più semplice*: profili on-device (già fatto), selezione Marca→Modello
guidata (già fatta), zero account/rete in auto. La strada non è eguagliare il
loro database, ma coprire i **PID gauge** che servono davvero (oil/trans/boost/
coolant/SOC/hv-temp) con un'esperienza immediata. La compatibilità "universale"
viene dal **ciclo protocollo + Mode 01 standard**, non dal database esteso.
