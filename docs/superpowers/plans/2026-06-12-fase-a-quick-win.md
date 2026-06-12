# Fase A — Quick win firmware: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pulsante reattivo (debounce 200→30ms), rimozione completa della tab Live (web UI + endpoint + plumbing), rimozione del fallback legacy `/profiles/<file>.json`.

**Architecture:** Tre rimozioni/modifiche indipendenti su firmware ESP32-C3 Arduino + web UI embedded. Nessun nuovo componente. La web UI sorgente è `web/index.html`; dopo ogni modifica va rigenerato `carmetrix/src/web_index.h` con `tools/embed_web.ps1`.

**Tech Stack:** Arduino ESP32 core v3, AsyncWebServer, ArduinoJson v7, LittleFS, web UI vanilla JS embedded gzippata.

**Vincoli di processo (dal CLAUDE.md e dalle preferenze utente):**
- ⚠️ **NON lanciare `arduino-cli compile` di iniziativa**: la compilazione/flash la fa l'utente dall'IDE (memoria progetto). I task di verifica firmware si chiudono chiedendo all'utente di compilare, oppure con sua autorizzazione esplicita alla CLI.
- La verifica della web UI si fa in locale con `tools/serve_web.ps1` (porta 8765) + preview browser, PRIMA di rigenerare `web_index.h`.
- Niente test framework su questo progetto embedded: la "verifica" è compilazione pulita + ispezione comportamento (preview per la UI, log seriale in auto per il firmware).

---

### Task 1: Fix reattività pulsante

**Files:**
- Modify: `carmetrix/src/config.h:61`

- [ ] **Step 1: Cambia il debounce**

In `carmetrix/src/config.h` sostituire:

```cpp
#define BTN_DEBOUNCE      200   // ms
```

con:

```cpp
#define BTN_DEBOUNCE      30    // ms — tap riconosciuto al rilascio; 200ms scartava i click veloci
```

- [ ] **Step 2: Verifica della logica (lettura, nessun run)**

In `carmetrix/carmetrix.ino` `handleButton()` (riga ~138) il click scatta al rilascio con
`held > BTN_DEBOUNCE && held < BTN_LONG_PRESS`. Con 30ms: tap normale (~50-150ms) → cambio
schermata immediato; le soglie 3s (config) e 6s (reset) usano costanti separate e NON cambiano.
Confermare leggendo che nessun altro punto usa `BTN_DEBOUNCE`:

Run: `Grep pattern="BTN_DEBOUNCE" path="carmetrix" output_mode="content"`
Expected: solo `config.h` (define) e `carmetrix.ino:138` (confronto al rilascio).

- [ ] **Step 3: Commit**

```bash
git add carmetrix/src/config.h
git commit -m "fix: debounce pulsante 200ms -> 30ms, click immediato"
```

---

### Task 2: Rimozione tab Live dalla web UI

**Files:**
- Modify: `web/index.html` (5 blocchi da eliminare)
- Regenerate: `carmetrix/src/web_index.h` (via `tools/embed_web.ps1`)

- [ ] **Step 1: Elimina il blocco CSS Live** (`web/index.html:175-187`)

Eliminare per intero:

```css
/* ── Live ── */
.live-item{margin-bottom:1.05rem}
.live-head{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:.3rem}
.live-name{font-family:var(--mono);font-size:.62rem;letter-spacing:.2em;color:var(--dim);text-transform:uppercase}
.live-val{font-family:var(--mono);font-size:1.35rem;color:#fff}
.live-val .u{font-size:.66rem;color:var(--dim);margin-left:.3rem}
```
…e le restanti regole `.live-track`, `.live-fill`, `.live-fill.warn`, `.live-fill.danger`,
`.live-item.novalid`, `.live-offline` (fino alla riga 187 inclusa).

- [ ] **Step 2: Elimina il pannello** (`web/index.html:297-305`)

```html
<!-- ═══════════ LIVE ═══════════ -->
<div id="panel-dashboard" class="panel">
  <div class="card">
    <div class="card-title">Telemetria <span id="live-age" style="float:right;color:var(--dim)"></span></div>
    <div id="live-grid">
      <div class="live-offline">In attesa di dati... (attiva Demo Mode o collega l'OBD)</div>
    </div>
  </div>
</div>
```

- [ ] **Step 3: Elimina la voce dalla bottom nav** (`web/index.html:422-425`)

```html
  <div class="nav-it" data-panel="dashboard" onclick="tab('dashboard')">
    <svg viewBox="0 0 24 24"><path d="M3 13h4l2-7 4 14 2-7h6"/></svg>
    Live
  </div>
```

- [ ] **Step 4: Pulisci `tab()`** (`web/index.html:447-448`)

Eliminare le due righe:

```js
  if (name === 'dashboard') startLivePolling();
  else                      stopLivePolling();
```

- [ ] **Step 5: Elimina l'intera sezione JS Live** (`web/index.html:888-948`)

Dal commento `// ── Live ──...` fino alla chiusura di `renderLive()` inclusa:
costante `LIVE_METRICS`, `liveTimer`, `startLivePolling()`, `stopLivePolling()`,
`pollLive()`, `renderLive()`.

- [ ] **Step 6: Verifica residui**

Run: `Grep pattern="live|dashboard" path="web/index.html" -i output_mode="content"`
Expected: nessuna occorrenza (eventuali falsi positivi tipo "delivered" sono ok, ma oggi non ce ne sono).

- [ ] **Step 7: Verifica visuale con il preview locale**

Run: `powershell tools/serve_web.ps1` (porta 8765), poi preview browser a 375px su `http://localhost:8765`.
Expected: 4 tab (Setup/Display/Alert/Update), navigazione ok su tutte, zero errori in console
(in particolare nessun `startLivePolling is not defined`).

- [ ] **Step 8: Rigenera l'header embedded**

Run: `powershell tools/embed_web.ps1`
Expected: `carmetrix/src/web_index.h` rigenerato, dimensione gzip < ~10.8KB attuale (messaggio dello script).

- [ ] **Step 9: Commit**

```bash
git add web/index.html carmetrix/src/web_index.h
git commit -m "feat: rimossa tab Live dalla web app (inutile: hotspot spento durante BLE)"
```

---

### Task 3: Rimozione endpoint /api/live e plumbing

**Files:**
- Modify: `carmetrix/src/web_server.cpp` (righe 19, 176-201, 430-432)
- Modify: `carmetrix/src/web_server.h:20`
- Modify: `carmetrix/carmetrix.ino:174`

- [ ] **Step 1: Elimina l'handler** (`web_server.cpp:176-201`)

Eliminare per intero il blocco:

```cpp
  // ─────────────────────────────────────────────────────────
  //  API: dati live (dashboard) — legge la struttura del main
  // ─────────────────────────────────────────────────────────
  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    if (liveData) {
      auto add = [&](const char* key, const OBDValue& v) { ... };
      add("iat", liveData->iat);
      ... tutte le add() ...
      doc["age"] = (millis() - liveData->lastUpdateMs);
    }
    String out;
    serializeJson(doc, out);
    jsonReply(req, 200, out);
  });
```

- [ ] **Step 2: Elimina lo stato e il setter**

In `web_server.cpp` eliminare la riga 19:

```cpp
static OBDData*  liveData      = nullptr;
```

e la definizione (righe ~430-432):

```cpp
void WebServer::setLiveData(OBDData* data) {
  liveData = data;
}
```

In `web_server.h` eliminare la riga 20:

```cpp
  void setLiveData(OBDData* data);
```

- [ ] **Step 3: Elimina la chiamata nel main** (`carmetrix.ino:174`)

```cpp
  WebServer::setLiveData(&obdData);
```

- [ ] **Step 4: Verifica residui**

Run: `Grep pattern="liveData|setLiveData|api/live" path="carmetrix" output_mode="content"`
Expected: zero occorrenze. Se `web_server.cpp` non usa più `OBDData`/`OBDValue`, valutare se
l'include relativo (probabilmente `obd_decoder.h`) serve ancora per altro prima di toglierlo.

- [ ] **Step 5: Commit**

```bash
git add carmetrix/src/web_server.cpp carmetrix/src/web_server.h carmetrix/carmetrix.ino
git commit -m "feat: rimosso endpoint /api/live e plumbing setLiveData"
```

---

### Task 4: Rimozione fallback legacy /profiles/<file>.json

**Files:**
- Modify: `carmetrix/src/profile_loader.cpp:27-92`

- [ ] **Step 1: Sostituisci la coda di `load()`**

In `profile_loader.cpp`, il ramo bundle (righe 39-63) resta INVARIATO (incluso il fallback
interno a `generic` e lo strip di `.json` per retro-compatibilità della config NVS).
Eliminare tutto il blocco legacy (righe 65-92, dal commento `// ── Legacy: file singolo...`
fino alla chiusura) e sostituirlo con:

```cpp
  Serial.println("[Profile] /profiles.json mancante su LittleFS — nessun profilo caricato");
  return false;
}
```

- [ ] **Step 2: Controlla che nulla dipenda dal percorso legacy**

Run: `Grep pattern="/profiles/" path="carmetrix" output_mode="content"`
Expected: zero occorrenze nei sorgenti (i sorgenti git in `profiles/` root NON c'entrano:
quelli li usa solo il converter PowerShell).

- [ ] **Step 3: Controlla i chiamanti di `load()`**

Run: `Grep pattern="ProfileLoader::load" path="carmetrix" output_mode="content"`
Expected: i chiamanti gestiscono già il ritorno `false` (il comportamento senza bundle era
comunque "nessun profilo"). Nessuna modifica attesa; se un chiamante assumesse `true`, segnalarlo.

- [ ] **Step 4: Commit**

```bash
git add carmetrix/src/profile_loader.cpp
git commit -m "refactor: rimosso fallback legacy /profiles/<file>.json (bundle unico)"
```

---

### Task 5: Verifica di compilazione e chiusura

**Files:** nessuno (verifica)

- [ ] **Step 1: Compilazione**

Chiedere all'utente di compilare dall'IDE (board ESP32C3 Dev Module, partition
**Minimal SPIFFS 1.9MB**, **USB CDC On Boot = Enabled**) — oppure, SOLO con sua
autorizzazione esplicita, usare:

```powershell
& "C:\Users\aless\arduino-cli\arduino-cli.exe" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc" --build-path "$env:LOCALAPPDATA\arduino\carmetrix-cli-build" --output-dir carmetrix\build carmetrix
```

Expected: compilazione pulita, dimensione ≤ 1.669.321 byte (v0.3.3 di riferimento: le
rimozioni devono far scendere, non salire).

- [ ] **Step 2: Annotare la dimensione e aggiornare CLAUDE.md**

Aggiornare la sezione "STATO ATTUALE" di `CLAUDE.md`: tab Live rimossa, debounce 30ms,
fallback legacy rimosso. Commit:

```bash
git add CLAUDE.md
git commit -m "docs: stato post fase A in CLAUDE.md"
```

- [ ] **Step 3: Release**

La release (bump versione, tag, GitHub) NON fa parte di questo piano: si fa con la skill
`/carmetrix-release` quando l'utente decide di chiudere la build da testare in auto
(presumibilmente insieme al layer v0.3.5 mai flashato).
