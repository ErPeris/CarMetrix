# Design — Prossime migliorie CarMetrix (giugno 2026)

Obiettivo dichiarato dall'utente: **CarMetrix deve essere preferibile a un'app
come Car Scanner** — stessa compatibilità con le auto, ma esperienza d'uso più
semplice e immediata. Le migliorie si organizzano in tre fasi indipendenti,
rilasciate e testate separatamente.

## Contesto e problemi rilevati

1. **Tab Live inutile**: durante il MONITORING l'hotspot è spento (coesistenza
   WiFi+BLE sul C3 rompe il BLE — lezione v0.2.5), quindi la tab Live non può
   mai mostrare dati reali. Funziona solo in DEMO, dove non serve.
2. **Pulsante poco reattivo**: `BTN_DEBOUNCE = 200ms` e azione riconosciuta solo
   al rilascio con `held > 200ms` → ogni tap sotto i 200ms viene scartato;
   l'utente deve tenere premuto ~1s perché il cambio schermata scatti.
3. **Flash all'84%** (1.67MB su 1.97MB): poco margine per TFT/feature future.
4. **Compatibilità auto**: la compatibilità di Car Scanner si regge su due gambe:
   (a) layer di connessione universale (init ELM robusto, auto-detect protocollo,
   quirk per adattatore), (b) database PID extra per marca. CarMetrix ha già
   172 profili OBDb (gamba b) ma il ciclo protocollo universale (gamba a) è
   solo in roadmap.

## Fase A — Quick win firmware (una release, basso rischio)

### A1. Fix reattività pulsante
- `BTN_DEBOUNCE`: 200 → 30 ms in `config.h` (pulsante a GND con INPUT_PULLUP:
  30ms bastano ampiamente).
- Il click resta riconosciuto al rilascio (necessario per distinguerlo dalla
  pressione lunga 3s/6s), ma con soglia a 30ms il cambio schermata è percepito
  come immediato.
- Verifica: tap rapidi ripetuti cambiano schermata senza scarti; 3s config mode
  e 6s reset confirm invariati.

### A2. Rimozione tab Live
- `web/index.html`: via la tab Live dalla tab bar (restano Setup/Display/Alert/
  Update), via il pannello e il JS di polling.
- `web_server.cpp`: via l'endpoint `/api/live` e il codice che serializza i
  dati live.
- Rigenerare `src/web_index.h` con `tools/embed_web.ps1`.
- L'OLED resta l'unica vista dati: coerente con l'architettura (radio al BLE
  durante il monitoring).

### A3. Pulizia codice morto contestuale
- Rimuovere il fallback legacy `/profiles/<file>` in `profile_loader` (il
  bundle `profiles.json` è la via ufficiale; i singoli file non esistono più
  in `data/`).
- Altri residui individuati strada facendo (solo rimozioni sicure e verificabili
  a compile-time; niente refactor strutturali in questa fase).
- Lo schema 1 dei profili NON si tocca finché non è verificato che nessun
  profilo del bundle lo usa.

### Release
- Bump versione, compile, release via `/carmetrix-release` (compila l'utente
  dall'IDE se preferisce — vedi memoria progetto).
- Si testa in auto insieme al layer v0.3.5 mai flashato.

## Fase B — Analisi APK Car Scanner (ricerca, zero rischio firmware)

L'utente fornisce l'APK. Analisi: estrazione zip (asset, database SQLite),
eventuale decompilazione con jadx se servono i formati.

**Perimetro legale concordato**: studiare l'organizzazione interna e fare
verifica mirata è interoperabilità legittima; NON si copia in blocco il
database PID proprietario nel repo pubblico. I singoli fatti tecnici
(PID/header/formule per le auto verificate) sono utilizzabili.

Output: documento di analisi con quattro sezioni:
1. **Connection profiles**: come Car Scanner struttura init/protocollo/quirk
   per agganciare qualsiasi auto → specifica per il nostro ciclo protocollo
   universale (ATSP0 → 6→7→8→9→3..5 con probe 0100, protocollo salvato in NVS).
2. **Organizzazione database PID**: gerarchia marca/modello, flow control,
   fallback → migliorie al nostro schema profili e al converter.
3. **Verifica mirata**: Civic 1.5T (ATF 222201@DA1D, init 29-bit fisico),
   toyota/yaris_hybrid (profilo manuale SPERIMENTALE), bmw_1_series (da forum).
   Correzioni applicate ai nostri profili se emergono discrepanze.
4. **Backlog derivato**: cosa adottare (es. ATFCSH per i profili WICAN fase 2),
   con priorità.

## Fase C — Flash budget: migrazione NimBLE (separata, alto impatto)

- Sostituire lo stack BLE Bluedroid con NimBLE-Arduino: stima -400KB flash
  (dall'84% a ~64%).
- Fatta da sola, dopo che A è stabile in auto: tocca `ble_elm327.*`, il cuore
  appena stabilizzato — se qualcosa si rompe deve essere inequivocabile che è
  stato NimBLE.
- Interfaccia pubblica di `BleElm327` invariata; cambia solo l'implementazione
  (client, discovery via advertised device + address type, notify/write su
  18F0/2AF0/2AF1, NIENTE pairing).
- Test in auto dedicato: connessione iCar Pro, stabilità polling, riconnessione.

## Decisioni prese (con l'utente)

- Tab Live: **rimossa del tutto** (non nascosta, non sostituita).
- Snellimento = **flash** + **codice morto** (la ristrutturazione dei file
  grossi non è in scope ora).
- APK: **verifica mirata + studio dell'organizzazione**, niente dump completo.
- Veicoli di riferimento per la verifica: Civic + i due profili manuali, ma
  l'obiettivo strategico è la compatibilità universale (gamba a).
- Ordine: A → B → C, mai mescolati nella stessa release.
