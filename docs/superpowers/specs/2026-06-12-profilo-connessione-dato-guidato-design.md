# Design — Profilo connessione dato-guidato (session + flow control)

Obiettivo: rendere il **Mode 22 abilitabile su qualsiasi auto via dati**,
aggiungendo al profilo JSON due campi opzionali per-PID — `session` (sessione
UDS da inviare prima delle query su una ECU) e `fc` (flow control per risposte
multi-frame) — senza patch firmware per auto. Modello derivato dall'analisi di
Car Scanner (campo `StartCommand`/sessione per ECU): vedi
`2026-06-12-fase-b-analisi-car-scanner.md`.

## Contesto: cosa esiste già

Lo **switch header per-PID è già dato-guidato**. In schema 2 ogni `ExtendedPID`
ha `hdr`/`rax`; `BleElm327::getHeaderCmds()` ne deriva `ATSH`/`ATCRA`, e
`OBDDecoder::pollTick` usa una mini-coda (`queueHeaderSwitch`) che invia quei
comandi uno-per-tick, non bloccante, prima della query vera. I PID sono già
raggruppati per `hdr` per minimizzare gli switch.

Ciò che **non** è dato-guidato e che serve per molti Mode 22:
- **Sessione diagnostica UDS**: alcune ECU rispondono al Mode 22 solo dopo una
  richiesta di sessione (es. `10 03` extended, `10 C0` custom). Oggi non c'è
  modo di esprimerla nel profilo.
- **Flow control** (`ATFCSH`/`ATFCSM`): necessario per le risposte ISO-TP
  multi-frame su certe ECU. Oggi assente.

## Fuori scope (deciso)

Rendere dato-guidato l'`init()` (detect protocollo, physical addressing
funzionale↔fisico) resta **escluso**: è ortogonale, già implementato in C++
(v0.3.4) e gonfierebbe lo scope. Eventuale lavoro separato futuro.

## Schema profilo: due campi opzionali

Aggiunti all'`ExtendedPID` (schema 2). **Default assenti = comportamento
identico a oggi.**

```json
{ "cmd": "222201", "hdr": "DA1D", "rax": "1D", "target": "trans_temp",
  "bix": 24, "len": 8, "add": -40,
  "session": "1003",
  "fc": "DA1DF1" }
```

- **`session`** (stringa, opz): una richiesta inviata come query prima delle
  interrogazioni su quella ECU, **risposta ignorata** (fire-and-forget). È lo
  `StartCommand` di Car Scanner. Esempi: `"1003"`, `"10C0"`.
- **`fc`** (stringa, opz): header per il flow control. Se presente, il firmware
  emette `ATFCSH<fc>` + `ATFCSM1` per gestire le risposte multi-frame.

Granularità: **per-PID** (deciso). Se due PID condividono la stessa ECU la
stringa si ripete — costo trascurabile (1-3 PID estesi per profilo). Niente
strutture nuove nel JSON.

## Modifiche al codice

### `profile_loader.*`
- `ExtendedPID` guadagna `char session[8]` e `char fc[8]` (default `""`).
- `parsePids()` legge `p["session"]` e `p["fc"]` (entrambi opzionali).

### `obd_decoder.cpp` — coda di switch
- `pendingCmds[3]` → `pendingCmds[6]` (può contenere: ATSH, ATCRA, ATFCSH,
  ATFCSM1, session — max 5).
- `queueHeaderSwitch` riceve anche il `PollItem` corrente (per accedere a
  `session`/`fc` del PID via `extIdx`). Dopo i comandi header:
  1. se `fc[0]`: accoda `"ATFCSH" + fc` e `"ATFCSM1"`;
  2. se `session[0]`: accoda la stringa `session` (query, non comando AT).
- L'ordine garantisce: header impostato → flow control impostato → sessione
  aperta → query Mode 22. La coda resta uno-per-tick, non bloccante.
- I comandi sono inviati **solo allo switch di header** (quando `hdr` cambia
  rispetto all'attivo), non a ogni query: la sessione/fc per una ECU si
  (ri)apre quando si entra in quella ECU nel giro di polling.

### `ble_elm327.*`
- Nessuna firma nuova obbligatoria: `ATFCSH`/`ATFCSM1`/session passano per il
  meccanismo `sendCommand` già usato dalla coda. Se serve un helper
  `appendSessionFc()` resta dettaglio implementativo del piano.

## Flusso dati (esempio Civic ATF, se richiede sessione)

```
giro polling … PID 222201 (hdr DA1D, session 1003, fc DA1DF1)
 hdr attivo ≠ DA1D → queueHeaderSwitch:
   tick1: ATSHDA1DF1
   tick2: ATCRA18DAF11D
   tick3: ATFCSHDA1DF1
   tick4: ATFCSM1
   tick5: 1003           (risposta 5003… ignorata)
 hdr ora DA1D → query reale:
   tick6: 222201 → 6222 01 <payload> → decode bix/len → trans
```

Se `session`/`fc` assenti (tutti i profili OBDb): tick3-5 saltati, identico a
oggi.

## Chi popola i campi

- **OBDb (172 profili)**: vuoti. OBDb non conosce le sessioni → funzionano come
  oggi (solo header switch).
- **Profili manuali** (es. `honda/civic`): popolati quando la verifica in auto
  col **ponte seriale** rivela che una ECU richiede sessione/flow control. Il
  test Civic `222201` dirà se l'ATF ha bisogno di `1003`.

Il meccanismo è **infrastruttura additiva**: non indovina le sessioni, offre i
campi per registrarle quando si scoprono in auto. La "generalità" è nella
*capacità* dello schema, popolata via verifica empirica.

## Verifica

Niente test automatici (firmware embedded). Verifica:
1. **Compile pulita** (utente da IDE) e dimensione firmware non superiore in
   modo significativo (atteso +poche centinaia di byte).
2. **Profili OBDb invariati**: un profilo senza `session`/`fc` produce
   esattamente la stessa sequenza di comandi di prima (verificabile a tavolino
   leggendo il log seriale: nessun ATFCSH/sessione tra le query).
3. **In auto, Civic**: aggiungere `session`/`fc` a `honda/civic` `222201` e
   osservare nel log la sequenza tick1-6 e una risposta `6222…` non più NO DATA
   (conferma il valore TRANS in Car Scanner come riferimento).

**Follow-up in auto (manuale, fuori dal piano firmware).** Il valore concreto
di `session`/`fc` per la Civic si scopre col **ponte seriale**: inviare a mano
`1003` poi `222201` e vedere se l'ATF risponde (e con quale eventuale flow
control). Se confermato, aggiungerlo a `profiles/honda/civic.json`, rigenerare
il bundle col converter (`tools/obdb_to_profile.ps1`) e ricaricare LittleFS.

## Rischi e mitigazioni

- **Coda più lunga** → switch di ECU leggermente più lento (5 tick invece di 2).
  Mitigato dal raggruppamento per header già esistente: si paga una volta per
  ECU per giro, non per PID.
- **Sessione che scade**: se l'ECU chiude la sessione tra un giro e l'altro, si
  riapre al successivo switch di header (la sessione è legata allo switch, non
  inviata una sola volta al boot). Accettabile.
- **`fc` errato** può rompere la ricezione di quella ECU: campo opzionale,
  popolato solo dove verificato. I profili OBDb non lo usano.
