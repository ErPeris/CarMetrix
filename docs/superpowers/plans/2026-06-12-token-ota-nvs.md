# Token OTA in NVS — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Memorizzare il GitHub token per l'OTA da repo privata in NVS (impostabile dalla web app), così i `.bin` rilasciati restano puliti e il token sopravvive agli OTA.

**Architecture:** Mirror del pattern `homePass` esistente: campo in `CarMetrixConfig`, mai esposto nel GET (`hasGithubToken`), aggiornato solo se arriva un valore nuovo non vuoto. `github_ota` legge da NVS a runtime con fallback al macro `GITHUB_TOKEN` (secrets.h). Web UI: campo password nella tab Update.

**Tech Stack:** Arduino ESP32 core v3, Preferences (NVS), AsyncWebServer + AsyncJson, web UI embedded.

**Vincoli di processo:** NON compilare di iniziativa (utente da IDE, o sua autorizzazione esplicita). Web UI verificata col preview locale prima di rigenerare `web_index.h`.

**Riferimenti (già letti):**
- struct + namespace: `carmetrix/src/nvs_config.h:9-32`
- `github_ota` lettura token: `carmetrix/src/github_ota.cpp:11-24` (macro), `:~58-75` (uso `hasToken`)
- config GET `hasWifiPass`: `carmetrix/src/web_server.cpp:168-169`
- config save `homePass`: `carmetrix/src/web_server.cpp:195-198`
- tab Update web: `web/index.html` sezione `panel-ota` (WiFi di casa)

---

### Task 1: Campo `githubToken` in NVS

**Files:**
- Modify: `carmetrix/src/nvs_config.h:9-21`

- [ ] **Step 1: Aggiungi il campo alla struct**

In `carmetrix/src/nvs_config.h`, dentro `struct CarMetrixConfig`, dopo
`char homePass[65];` aggiungi:

```cpp
  char     homePass[65];     // password WiFi di casa
  char     githubToken[128]; // GitHub PAT per OTA da repo privata ("" = repo pubblica)
```

Nota: `load()` fa già `memset` e azzera se la size del blob non corrisponde
(`nvs_config.cpp:8,15`) → il nuovo campo parte vuoto e il cambio layout forza il
setup-da-rifare dopo flash (atteso).

- [ ] **Step 2: Commit**

```bash
git add carmetrix/src/nvs_config.h
git commit -m "feat: campo githubToken in CarMetrixConfig (NVS)"
```

---

### Task 2: OTA legge il token da NVS

**Files:**
- Modify: `carmetrix/src/github_ota.cpp` (blocco macro `GITHUB_TOKEN` + `hasToken`)

- [ ] **Step 1: Token da NVS con fallback al macro**

In `carmetrix/src/github_ota.cpp`, `run()` carica già `cfg` via
`NVSConfig::load(cfg)` (riga ~39). Sostituisci la riga:

```cpp
  const bool hasToken = (sizeof(GITHUB_TOKEN) > 1);  // "" → sizeof 1
```

con (NVS prioritario, fallback al macro compile-time):

```cpp
  const char* token = cfg.githubToken[0] ? cfg.githubToken : GITHUB_TOKEN;
  const bool  hasToken = (token[0] != '\0');
```

- [ ] **Step 2: Usa `token` al posto del macro nelle Authorization**

Nello stesso file, sostituisci le due occorrenze dell'header con il macro:

```cpp
    if (hasToken) https.addHeader("Authorization", "Bearer " GITHUB_TOKEN);
```
→
```cpp
    if (hasToken) https.addHeader("Authorization", String("Bearer ") + token);
```

e

```cpp
    h.addHeader("Authorization", "Bearer " GITHUB_TOKEN);
```
→
```cpp
    h.addHeader("Authorization", String("Bearer ") + token);
```

- [ ] **Step 3: Verifica residui macro**

Run: `Grep pattern="Bearer . GITHUB_TOKEN|\"Bearer \" GITHUB_TOKEN" path="carmetrix/src/github_ota.cpp" output_mode="content"`
Expected: nessuna occorrenza (tutte sostituite con la variabile `token`). Il
macro `GITHUB_TOKEN` resta solo nel calcolo di `token` come fallback.

- [ ] **Step 4: Commit**

```bash
git add carmetrix/src/github_ota.cpp
git commit -m "feat: OTA legge githubToken da NVS (fallback a secrets.h)"
```

---

### Task 3: Web server — espone stato e salva il token

**Files:**
- Modify: `carmetrix/src/web_server.cpp:168-169` (GET), `:195-198` (save)

- [ ] **Step 1: Esponi `hasGithubToken` nel GET config**

In `carmetrix/src/web_server.cpp`, dopo la riga `doc["hasWifiPass"] = ...`
(riga 169) aggiungi:

```cpp
    doc["hasWifiPass"] = (cfg.homePass[0] != '\0');  // non esporre la password
    doc["hasGithubToken"] = (cfg.githubToken[0] != '\0');  // non esporre il token
```

- [ ] **Step 2: Salva il token solo se nuovo e non vuoto**

Nel save handler, dopo il blocco `homePass` (righe 195-198) aggiungi:

```cpp
        // Aggiorna il token solo se ne arriva uno nuovo non vuoto
        if (obj["githubToken"].is<const char*>()) {
          const char* t = obj["githubToken"];
          if (t && t[0] != '\0') strlcpy(cfg.githubToken, t, sizeof(cfg.githubToken));
        }
```

- [ ] **Step 3: Commit**

```bash
git add carmetrix/src/web_server.cpp
git commit -m "feat: API config espone hasGithubToken e salva githubToken"
```

---

### Task 4: Web UI — campo token nella tab Update

**Files:**
- Modify: `web/index.html` (card "WiFi di casa" nel panel-ota + JS load/save)
- Regenerate: `carmetrix/src/web_index.h`

- [ ] **Step 1: Aggiungi il campo nella tab Update**

In `web/index.html`, nella card OTA dopo il blocco "Aggiorna da GitHub"
(la card con `id="btn-check"`), aggiungi una nuova card prima di
"Aggiorna da file .bin":

```html
  <div class="card">
    <div class="card-title">Token GitHub <span style="font-size:.6rem;color:var(--dim)">(repo privata)</span></div>
    <div class="form-group">
      <label>Personal Access Token <span id="gh-tok-status" style="color:var(--cyan)"></span></label>
      <input type="password" id="gh-token" placeholder="(incolla per impostarlo)">
    </div>
    <div class="hint">Serve solo se la repo CarMetrix è privata. Token fine-grained, Contents read-only sul solo repo. Resta nel dispositivo, non viaggia con l'OTA.</div>
    <button class="btn btn-cyan" onclick="saveToken()">▸ Salva token</button>
  </div>
```

- [ ] **Step 2: JS — stato del token nel load**

In `web/index.html`, nella funzione che carica la config (quella che usa
`hasWifiPass`, cerca `hasWifiPass` nel JS), dopo aver gestito `hasWifiPass`
aggiungi la riga che aggiorna lo stato del token:

```js
  document.getElementById('gh-tok-status').textContent = d.hasGithubToken ? '— impostato' : '';
```

(Se la funzione non referenzia ancora `hasWifiPass` nel JS, inserisci la riga
dentro il `.then(d => { ... })` del fetch `/api/config`.)

- [ ] **Step 3: JS — funzione saveToken**

Vicino alle altre funzioni di salvataggio (es. `saveWifi`), aggiungi:

```js
async function saveToken() {
  const t = document.getElementById('gh-token').value;
  if (!t) { toast('Inserisci un token', 'var(--amber)'); return; }
  await fetch('/api/config/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ githubToken: t, noReboot: true })
  });
  document.getElementById('gh-token').value = '';
  document.getElementById('gh-tok-status').textContent = '— impostato';
  toast('Token salvato');
}
```

Nota: l'endpoint di save è quello già usato da `saveWifi`. Verifica nel JS il
path reale (`/api/config/save`) e usalo identico qui.

- [ ] **Step 4: Verifica preview locale**

Run: `mcp preview_start name="webui"`, poi a 375px aprire la tab Update.
Expected: card "Token GitHub" visibile, campo password, nessun errore console
(`saveToken is not defined`).

- [ ] **Step 5: Rigenera l'header embedded**

Run: `powershell tools/embed_web.ps1`
Expected: `web_index.h` rigenerato senza errori.

- [ ] **Step 6: Commit**

```bash
git add web/index.html carmetrix/src/web_index.h
git commit -m "feat: campo token GitHub nella tab Update della web app"
```

---

### Task 5: Verifica e documentazione

- [ ] **Step 1: Compilazione**

Chiedere all'utente di compilare dall'IDE (o, con autorizzazione esplicita,
arduino-cli con `--build-path` dedicato — vedi CLAUDE.md). Expected: build
pulita, dimensione ~+128 byte di NVS + poche righe.

- [ ] **Step 2: Aggiorna CLAUDE.md**

Aggiungere alla sezione STATO ATTUALE: il token OTA per repo privata si imposta
dalla tab Update (NVS, `hasGithubToken`), con fallback a `secrets.h`; sopravvive
agli OTA, i `.bin` restano puliti. Nota NVS: blob cambiato → setup da rifare.

```bash
git add CLAUDE.md
git commit -m "docs: token OTA in NVS in CLAUDE.md"
```

---

## Self-review

- **Spec coverage:** campo NVS → Task 1; OTA legge NVS+fallback → Task 2; GET
  `hasGithubToken` + save → Task 3; web UI + regen → Task 4; compile+doc → Task 5.
- **Placeholder scan:** nessun TBD; codice completo in ogni step.
- **Type consistency:** `githubToken[128]` identico in struct (Task 1), OTA
  (Task 2), web server (Task 3). Chiave JSON `githubToken` coerente tra save
  handler (Task 3) e JS (Task 4). `hasGithubToken` coerente GET (Task 3) ↔ JS (Task 4).
