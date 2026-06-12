# Profilo connessione dato-guidato (session + flow control) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aggiungere due campi opzionali per-PID (`session`, `fc`) allo schema 2 dei profili e farli inviare dalla coda di switch header, così il Mode 22 si abilita via dati senza patch firmware per auto.

**Architecture:** Solo firmware. `ExtendedPID` guadagna `session`/`fc` (default `""`). La coda esistente `queueHeaderSwitch` in `obd_decoder.cpp` — che già invia ATSH/ATCRA uno-per-tick allo switch di ECU — accoda dopo di essi `ATFCSH<fc>`+`ATFCSM1` (se `fc`) e la query `session` (se presente). Additivo: profili senza i campi → sequenza identica a oggi.

**Tech Stack:** Arduino ESP32 core v3, ArduinoJson v7, LittleFS bundle profili, ELM327 via BLE.

**Vincoli di processo:**
- ⚠️ **NON lanciare `arduino-cli compile` di iniziativa**: compila/flasha l'utente dall'IDE (memoria progetto). Il task di verifica si chiude chiedendo all'utente, o con sua autorizzazione esplicita alla CLI.
- Niente test framework (firmware embedded): la verifica è compilazione pulita + ispezione log seriale + ragionamento a tavolino sull'invarianza dei profili OBDb.
- **Scope = meccanismo firmware**. Scoprire QUALE `session`/`fc` serve alla Civic è follow-up manuale in auto (ponte seriale), fuori da questo piano. Nessun profilo viene modificato qui.

**Riferimenti codice (stato attuale, già letto):**
- `ExtendedPID` struct: `carmetrix/src/profile_loader.h:27-43`
- `parsePids` schema 2: `carmetrix/src/profile_loader.cpp:82-94`
- coda switch: `carmetrix/src/obd_decoder.cpp:204-230` (`pendingCmds[3]`, `queueHeaderSwitch`)
- call site reale (query con hdr): `carmetrix/src/obd_decoder.cpp:443-447`
- call site standby (no PID): `carmetrix/src/obd_decoder.cpp:361-362`
- `getHeaderCmds`/`getDefaultHeaderCmds`: `carmetrix/src/ble_elm327.cpp:509-527`

---

### Task 1: Campi `session`/`fc` nel modello PID

**Files:**
- Modify: `carmetrix/src/profile_loader.h:27-43`
- Modify: `carmetrix/src/profile_loader.cpp:82-94`

- [ ] **Step 1: Aggiungi i campi alla struct**

In `carmetrix/src/profile_loader.h`, dentro `struct ExtendedPID`, dopo
`char rax[6];` (riga 34) aggiungi:

```cpp
    char      rax[6];        // filtro risposta ("1D", "7E8")
    char      session[8];    // opz: sessione UDS inviata allo switch header ("1003"); "" = nessuna
    char      fc[8];         // opz: header flow-control per multi-frame; "" = nessuno
```

- [ ] **Step 2: Leggi i campi in parsePids (solo schema 2)**

In `carmetrix/src/profile_loader.cpp`, nel ramo `if (schema >= 2)`, subito dopo
`strlcpy(ep.rax, p["rax"] | "", sizeof(ep.rax));` (riga 89) aggiungi:

```cpp
      strlcpy(ep.rax, p["rax"] | "", sizeof(ep.rax));
      strlcpy(ep.session, p["session"] | "", sizeof(ep.session));
      strlcpy(ep.fc,      p["fc"]      | "", sizeof(ep.fc));
```

Nota: `ProfileLoader::ExtendedPID ep = {};` (riga 73) azzera già tutto, quindi
per i profili senza i campi (tutti gli OBDb e lo schema 1) `session`/`fc`
restano `""`.

- [ ] **Step 3: Commit**

```bash
git add carmetrix/src/profile_loader.h carmetrix/src/profile_loader.cpp
git commit -m "feat: campi opzionali session/fc nello schema 2 dei profili"
```

---

### Task 2: La coda di switch header invia fc + session

**Files:**
- Modify: `carmetrix/src/obd_decoder.cpp:207` (array)
- Modify: `carmetrix/src/obd_decoder.cpp:217-230` (`queueHeaderSwitch`)
- Modify: `carmetrix/src/obd_decoder.cpp:443-447` (call site con PID)

- [ ] **Step 1: Allarga l'array dei comandi pendenti**

In `carmetrix/src/obd_decoder.cpp` riga 207, sostituisci:

```cpp
static String  pendingCmds[3];
```

con (capienza: ATSH, ATCRA, ATFCSH, ATFCSM1, session = max 5):

```cpp
static String  pendingCmds[6];
```

- [ ] **Step 2: queueHeaderSwitch accetta e accoda fc/session**

Sostituisci l'intera funzione `queueHeaderSwitch` (righe 217-230) con:

```cpp
// true = switch accodato (chiamante deve attendere i prossimi tick).
// session/fc opzionali (profili schema 2): dopo ATSH/ATCRA si accodano
// ATFCSH<fc>+ATFCSM1 (flow control multi-frame) e la query di sessione UDS.
static bool queueHeaderSwitch(const char* want, const char* rax,
                              const char* session = "", const char* fc = "") {
  pendingTarget = want;
  int n = want[0]
    ? BleElm327::getHeaderCmds(want, rax, pendingCmds)
    : BleElm327::getDefaultHeaderCmds(pendingCmds);
  if (n == 0) {  // protocollo senza header gestiti: nulla da fare
    pendingCount = 0;
    BleElm327::setActiveHdr(want);
    return false;
  }
  if (fc && fc[0] && n <= 4) {           // flow control: 2 comandi AT
    pendingCmds[n++] = String("ATFCSH") + fc;
    pendingCmds[n++] = "ATFCSM1";
  }
  if (session && session[0] && n <= 5)   // sessione UDS: 1 query (risposta ignorata)
    pendingCmds[n++] = session;
  pendingCount = n;
  pendingIdx = 0;
  pendingAwaiting = false;
  return true;
}
```

Nota: `getHeaderCmds`/`getDefaultHeaderCmds` scrivono al massimo gli indici 0-2;
passare `pendingCmds` (ora `[6]`) è sicuro. Le guardie `n <= 4`/`n <= 5` evitano
overflow anche nel caso teorico di 3 comandi header di default + fc + session.

- [ ] **Step 3: Passa session/fc dal call site con PID**

In `carmetrix/src/obd_decoder.cpp`, nel loop di invio query (righe 443-447),
sostituisci:

```cpp
    const char* want = itemHdr(it);
    if (strcmp(want, BleElm327::activeHdr()) != 0) {
      const char* rax = (it.extIdx >= 0)
        ? ProfileLoader::getExtendedPIDs()[it.extIdx].rax : "";
      if (queueHeaderSwitch(want, rax)) return;
    }
```

con:

```cpp
    const char* want = itemHdr(it);
    if (strcmp(want, BleElm327::activeHdr()) != 0) {
      const char* rax = "", *session = "", *fc = "";
      if (it.extIdx >= 0) {
        const auto& ep = ProfileLoader::getExtendedPIDs()[it.extIdx];
        rax = ep.rax; session = ep.session; fc = ep.fc;
      }
      if (queueHeaderSwitch(want, rax, session, fc)) return;
    }
```

Il call site standby (riga 362, `queueHeaderSwitch("", "")`) resta invariato:
i parametri `session`/`fc` hanno default `""`.

- [ ] **Step 4: Verifica a tavolino dell'invarianza OBDb**

Run: `Grep pattern="queueHeaderSwitch" path="carmetrix/src/obd_decoder.cpp" output_mode="content" -n=true`
Expected: due call site — quello standby con due argomenti, quello query con
quattro. Confermare ragionando: per un PID con `session=""`/`fc=""` (tutti gli
OBDb) i due `if` interni non scattano → `pendingCmds` contiene solo ATSH/ATCRA
esattamente come prima.

- [ ] **Step 5: Commit**

```bash
git add carmetrix/src/obd_decoder.cpp
git commit -m "feat: coda switch header invia ATFCSH/ATFCSM1 + sessione UDS (schema 2)"
```

---

### Task 3: Verifica di compilazione e documentazione

**Files:**
- Modify: `CLAUDE.md` (sezione STATO ATTUALE)
- Modify: `docs/superpowers/specs/2026-06-12-profilo-connessione-dato-guidato-design.md` (nota follow-up)

- [ ] **Step 1: Compilazione**

Chiedere all'utente di compilare dall'IDE (ESP32C3 Dev Module, partition
Minimal SPIFFS, USB CDC On Boot = Enabled) — oppure, SOLO con sua
autorizzazione esplicita:

```powershell
& "C:\Users\aless\arduino-cli\arduino-cli.exe" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" --build-path "$env:LOCALAPPDATA\arduino\carmetrix-cli-build" --output-dir carmetrix\build carmetrix
```

Expected: compilazione pulita; dimensione attesa +poche centinaia di byte
rispetto alla build precedente (due `char[8]` per PID + qualche riga).

- [ ] **Step 2: Aggiorna CLAUDE.md**

Nella sezione "STATO ATTUALE" di `CLAUDE.md` aggiungere una voce: schema 2 ora
supporta `session` (sessione UDS allo switch header) e `fc` (ATFCSH/ATFCSM1 per
multi-frame), opzionali, default vuoti = invariato per gli OBDb. Da popolare
sui profili manuali dopo verifica in auto.

Commit:

```bash
git add CLAUDE.md
git commit -m "docs: schema 2 session/fc in CLAUDE.md"
```

- [ ] **Step 3: Annotare il follow-up in auto nel design doc**

In coda a `docs/superpowers/specs/2026-06-12-profilo-connessione-dato-guidato-design.md`
aggiungere una riga sotto "Verifica": il valore concreto di `session`/`fc` per
la Civic si scopre col **ponte seriale** (inviare `1003` poi `222201` a mano e
vedere se l'ATF risponde); se confermato, aggiungerlo a `profiles/honda/civic.json`,
rigenerare il bundle col converter e ricaricare LittleFS.

Commit:

```bash
git add docs/superpowers/specs/2026-06-12-profilo-connessione-dato-guidato-design.md
git commit -m "docs: nota follow-up verifica session/fc in auto"
```

---

## Self-review

- **Spec coverage:** schema (campi opzionali) → Task 1; esecuzione in coda
  (fc→session dopo ATSH/ATCRA, ordine, non-bloccante) → Task 2; invarianza
  OBDb → Task 2 Step 4; verifica compile + chi popola → Task 3. `init()` fuori
  scope: nessun task, coerente col design.
- **Placeholder scan:** nessun TBD/TODO; ogni step ha codice esatto.
- **Type consistency:** `session[8]`/`fc[8]` usati identici in struct (Task 1),
  parse (Task 1), e call site (Task 2). `queueHeaderSwitch` firma a 4 parametri
  con default coerente coi due call site.
