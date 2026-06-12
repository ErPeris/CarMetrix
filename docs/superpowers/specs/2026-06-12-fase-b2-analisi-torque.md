# Fase B2 — Analisi APK Torque Lite (torquefree 1.2.22) + confronto

Analisi statica dell'APK Torque Lite (5.7 MB) fornito dall'utente. Stessa
finalità di interoperabilità della Fase B (Car Scanner): capire architettura e
formato dati, non copiare. Tenuto solo in locale (`.gitignore: *.apk`).

## Metodo

Torque è un'app **Java classica** (un solo `classes.dex`, 5.6 MB; gli `assets/`
sono solo file di traduzione). Codice e definizioni PID sono nel dex. String
search diretta sul dex.

## Architettura: l'opposto di Car Scanner

| | Car Scanner | Torque Lite |
|---|---|---|
| Stack | .NET MAUI, DB scaricati a runtime | Java, tutto on-device |
| Profili veicolo | server `carscanner.info` (0 nell'APK) | PID standard nel codice + **custom editabili** |
| Formato PID | formule tokenizzate nei `.db` | **formula infissa A/B/C/D in chiaro** |
| Identificazione | fingerprint variante firmware ECU | nessuna: l'utente sceglie/aggiunge i PID |
| Apertura | chiuso, proprietario | **aperto**: CSV import/export, community |

**Torque NON scarica i profili**: i PID standard OBD2 sono nel codice, i PID
proprietari sono **custom PID editabili dall'utente** (`torquefree/pid/PID`,
`PIDEditor`, `PIDManagement`), salvati in un SQLite locale creato a runtime
(nessun `.db` bundled). Si importano/esportano via **CSV** — ed è qui il valore.

## Il formato Torque custom PID (rilevante per CarMetrix)

Campi del modello confermati dal dex: `equation`/`equationText`/`finalEquation`/
`subEquation`, `shortName`, `scale`/`scaleMult`, `getHeaderField(s)`, `Units`,
min/max. Comandi AT presenti: `ATSH7B0`, `ATCRA7B0`, `ATSP0`, `ATCAF0`, `ATMA`
→ supporta header per-PID e CAN filter, come noi.

**La formula è una stringa infissa con i byte di risposta A,B,C,D…** Esempi
reali estratti dal dex:
```
(A*7.988) + (B*0.0312)
((A-128*16)+8)+(B*0.0625)
A*0.13     A*12.5     A/40     A/10
```
A = primo byte dati dopo l'echo, B = secondo, ecc. È lo **standard de-facto**
della community: forum e siti pieni di custom PID Torque per ogni marca, nel
formato CSV:
`Name, ShortName, ModeAndPID, Equation, MinValue, MaxValue, Units, Header`.

## Confronto con lo schema CarMetrix (schema 2)

Oggi CarMetrix decodifica con campi numerici: `bix/len/sign/mul/div/add/min/max`
(bit offset + lunghezza + scala lineare). Limiti rispetto a Torque:
- copre solo trasformazioni **lineari** (mul/div/add). Le formule Torque tipo
  `((A-128*16)+8)+(B*0.0625)` o combinazioni non lineari non sono esprimibili.
- è più rigido ma **non richiede un parser** a bordo (leggero, deterministico).

Torque richiede un **parser di espressioni** (A/B/C/D + operatori), più potente
ma più pesante/fragile su ESP32.

## Conclusioni per i prossimi passi

Le due app danno indicazioni convergenti su una cosa e divergenti su un'altra:

1. **Convergenza — header/AT per-PID e CAN filter**: entrambe fanno ATSH/ATCRA
   per-PID. Conferma che il *profilo connessione dato-guidato* (backlog Fase B
   #1) è la strada giusta per il Mode 22.

2. **Divergenza — formato PID**: 
   - Car Scanner = database fingerprint-driven, irriproducibile senza il server.
   - **Torque = formato aperto, riproducibile**: i custom PID CSV sono una
     **seconda sorgente dati** (oltre a OBDb) per i PID Mode 22/21, già usata
     da migliaia di utenti e verificata sul campo. Importabili nel converter.

**Decisione aperta per CarMetrix:** se adottare il **formato formula stringa**
(à la Torque, con mini-parser a bordo) o restare sullo schema numerico attuale
(`bix/len/mul/div/add`) ed eventualmente importare i CSV Torque *convertendoli*
in offline nel converter quando la formula è lineare (la maggioranza dei gauge:
temp/press/% sono `A`, `A-40`, `A/256`…), scartando le non-lineari.

Raccomandazione: **non** mettere un parser di formule sull'ESP32 (peso/fragilità
sul budget flash all'84%). Meglio **estendere il converter** per ingerire i CSV
Torque della community e tradurli nello schema numerico quando possibile — così
si guadagna la sorgente dati senza appesantire il firmware. Le poche formule non
lineari restano fuori (o si gestiscono come caso speciale nel profilo).
