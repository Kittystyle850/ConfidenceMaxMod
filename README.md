# CT2CheatDLL

DLL x64 per **Captain Tsubasa 2: World Fighters** (offline).

## Effetti sempre attivi (default, zero tasti, zero suoni)
- **CHAIN MAX** — il CHAIN (condiviso, entrambe le squadre) resta al massimo
  dinamico consentito dalla modalità.
- **Unique Technique limit 2 → 4** — patch statica, 4 slot tecniche uniche
  normali invece di 2.

## Sblocco di tutte le mosse per tutti (nuovo, automatico)
Appena il salvataggio è pronto (anche fuori da una partita), la DLL sblocca
automaticamente **tutte e 353** le mosse/super mosse nell'inventario di
possesso del salvataggio — quelle a quantità 0 (bloccate) diventano 1
(possedute). Non tocca i giocatori uno per uno: agisce sull'inventario
globale, quindi ogni mossa sbloccata diventa selezionabile per **qualsiasi**
giocatore dal menu normale del gioco, non solo per il Custom Player.
Nessun tasto, una volta sola a sessione. Log:
`[+] MoveInventoryUnlocker: unlocked N/353 move cards...`

**Risultato del test (confermato in game): NON basta.** Con l'inventario
già al 100% posseduto, un giocatore normale (non Custom) mostra comunque
solo 2 mosse selezionabili per categoria, mentre il Custom Player ne vede
decine. Il possesso globale non è il filtro che decide cosa è selezionabile
per un personaggio specifico — probabilmente c'entra l'array di memoria
live che stavamo già cercando con F7/F8. L'unlock resta attivo (non fa
danno, è comunque un prerequisito), ma da solo non risolve l'obiettivo.

## Editor live del giocatore Custom
All'avvio la DLL genera, accanto all'exe del gioco:
- `CT2_MoveList_Reference.txt` — tutte le 431 mosse reali, organizzate a
  sezioni (Dribble/Tackle/Shot/AirShot/Saving/MiracleSaving/Super), un unico
  file invece di nove.
- `CT2_CustomPlayerMoves.txt` — il file che **tu** modifichi.

**Come si usa**: apri `CT2_CustomPlayerMoves.txt`, scrivi il nome esatto
(copia/incolla dalle lista `CT2_MoveList_Reference.txt`) accanto al campo che vuoi
cambiare, salva. La DLL rileva il salvataggio del file entro ~0.5s e applica
subito la mossa al giocatore Custom attivo — niente da riavviare, niente
tasti da premere.

```
Dribble=Marseille Turn
Tackle=Sliding Tackle
Shot=Drive Shot
AirShot=
Saving=
MiracleSaving=
SuperType=
SuperID=
```
Lascia un campo vuoto per non toccarlo.

### Nota importante: SuperType non è ancora confermato
Il file `.sav` salva il tipo di Super Move come **stringa di testo**
("SHOOT", "PASS", "DRIBBLE"...), ma la memoria live usa un **numero singolo
byte** (0,1,2,3,4,5,7,8). Non esiste, in nessuna delle fonti disponibili,
una mappatura verificata tra i due. Indovinarla avrebbe rischiato di
scrivere combinazioni corrotte, quindi per ora `SuperType`/`SuperID` vanno
inseriti come **numeri grezzi**, non nomi.

**Procedura di calibrazione** (da fare una volta sola, poi la blocco nel codice):
1. Apri `CT2_MoveList_Reference.txt`, sezione "Super" — contiene nome e ID per ogni mossa, ma
   non ancora il numero `SuperType` corretto.
2. Nel gioco, apri la schermata del giocatore Custom e nota la Super Move
   attualmente equipaggiata.
3. In `CT2_CustomPlayerMoves.txt`, prova `SuperType=0` con l'ID di una mossa
   che credi essere di quel tipo (es. un ID dalla lista `SHOOT`), salva,
   guarda se il gioco mostra quella mossa o un'altra/niente.
4. Ripeti con 1,2,3,4,5,7,8. Mandami quale numero mostra quale categoria —
   anche solo 2-3 conferme bastano per dedurre tutta la mappatura.

## Trappola di scrittura (F6, nuovo) — sostituisce la caccia al puntatore
L'array trovato con F7 cambia contenuto tra una visita e l'altra alla
stessa schermata: non è una tabella fissa, viene ricostruita al volo.
Inseguire un puntatore a qualcosa di effimero non serve. Soluzione più
solida: **catturare l'istruzione esatta che scrive** quel campo, nel
momento in cui lo scrive — come "Find out what writes to this address" di
Cheat Engine, fatto con una pagina `PAGE_GUARD` e un gestore eccezioni.

**Procedura**:
1. Premi **F7** su una schermata rosa (localizza un record e lo ricorda).
2. Subito dopo premi **F6** — arma la trappola sulla pagina di quel record.
3. Fai qualcosa in game che costringa quella lista a **ridisegnarsi**:
   scorri via e torna, cambia scheda e torna, esci e rientra dal menu.
4. Appena il gioco riscrive quella memoria, la trappola scatta una volta
   sola e scrive `CT2_WriteTrap_Report.txt` con: indirizzo esatto
   dell'istruzione che ha scritto (RVA dal modulo se dentro l'exe, quindi
   stabile), tutti i registri, e un dump di 64 byte di codice grezzo
   intorno a quell'istruzione.

Mandami quel file — da lì leggo l'istruzione reale e costruisco l'hook
definitivo, stesso procedimento usato per CHAIN MAX ma partendo da codice
vero invece che da una CT table già pronta.

## Dump sequenziale dell'array + caccia al puntatore stabile (F7)
Premi **F7** su una schermata rosa: ritrova Izawa da zero (gli indirizzi
sono in heap, cambiano ad ogni riavvio), stampa 15 record prima + 60 dopo
con CharaID risolto in nome reale, poi **insegue automaticamente la catena
di puntatori fino a 5 salti** cercando un indirizzo che stia dentro il
modulo del gioco (quello sì stabile, utilizzabile da `moduleBase+RVA` ad
ogni sessione). Scrive `CT2_ArrayDump_Report.txt`. Può metterci qualche
secondo in più del solito per via dei salti multipli — è normale.
Mandamelo: se trova un indirizzo marcato `*** STABLE ***`, abbiamo quello
che serve per scrivere l'hook vero.

## Caccia all'array di tutti i giocatori (nuovo)
Premi **F8** mentre sei su una schermata con la rosa visibile (formazione,
cambio giocatori, panchina). La DLL scansiona tutta la memoria del processo
cercando Izawa/Sawada/Hyuga (valori noti dal database) e segnala dove più
campi del loro moveset compaiono vicini tra loro — probabile record vero,
non rumore. Protetto con SEH: se una pagina di memoria sparisce a metà
scansione, salta quella zona invece di far crashare il gioco.

Output: `CT2_PlayerScan_Report.txt` (candidati con punteggio + dump
esadecimale/int32 dei dintorni) e `CT2_PlayerScan_Raw.txt` (conteggio grezzo
hit). Mandami `CT2_PlayerScan_Report.txt` e da lì ricavo offset/stride reali
per scrivere l'hook che edita tutta la rosa.

## Log
`CT2CheatDLL_log.txt`, accanto all'exe. `[+]` = ok, `[!]` = qualcosa non
trovato/non applicato (es. nome mossa scritto male, o menu custom-player non
aperto). `F9` scarica la DLL, `ESC` ferma il thread di controllo (gli
effetti restano comunque attivi).

## Limiti noti
- L'editor Custom Player modifica **solo il giocatore che stai creando/
  modificando nella schermata di editing**, non l'intera rosa dei 358
  giocatori — quella è la prossima fase (richiede trovare l'array di tutti
  i giocatori live in memoria, non ancora localizzato né dalla CT table né
  dal save editor, serve una sessione di scansione con Cheat Engine).
- Scrive su 3 posizioni (`save`, `avatarCom`, `editor`) quando disponibili,
  per mostrare il cambio subito; se il menu custom-player non è aperto,
  scrive solo su `save` (effetto visibile al prossimo caricamento/apertura
  del menu).

## Changelog
- **v2.1** — aggiunto l'editor live del giocatore Custom: catena di
  puntatori portata dallo script Lua dell'entry "Live Master Save Data
  Pointer" + "Load/Commit" della CT table (nessun AOB-scan, solo RVA fisse
  validate via UObject flags/classe), database di 431 mosse reali generato
  dal save editor Python allegato, file di config leggibile dall'utente con
  polling automatico.
- **v2.0** — rimossi Instant Tackle Charge, Super Dribble lock, Always
  PERFECT Dribble, Instant Shot Charge e i relativi hotkey/beep. Restano
  CHAIN MAX e Unique Technique Limit, entrambi forzati ON all'avvio.
- **v1.1** — fix trampolino vicino per quando la DLL si carica oltre i 2GB
  dal modulo del gioco (succede spesso con LoadLibrary/ASI loader): un
  piccolo stub `jmp qword ptr [...]` allocato entro range fa da ponte.

## Build
CMake + MASM (`ml64`, incluso nei runner `windows-latest` di GitHub
Actions) via `.github/workflows/build.yml`. Nessuna dipendenza esterna,
nessun framework di hooking: AOB-scan e jmp-hook scritti a mano in
`main.cpp`, un codecave in `hooks.asm`, e l'editor Custom Player in
`custom_player_editor.h` + `movedb.h` (tabelle mosse, generate dal JSON del
save editor — non modificare a mano, rigenerare se il database cambia).
