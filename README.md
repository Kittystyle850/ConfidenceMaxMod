# CT2CheatDLL

DLL x64 per **Captain Tsubasa 2: World Fighters** (offline).

## Effetti sempre attivi (default, zero tasti, zero suoni)
- **CHAIN MAX** — il CHAIN (condiviso, entrambe le squadre) resta al massimo
  dinamico consentito dalla modalità.
- **Unique Technique limit 2 → 4** — patch statica, 4 slot tecniche uniche
  normali invece di 2.

## Editor live del giocatore Custom (nuovo)
All'avvio la DLL genera, accanto all'exe del gioco:
- `CT2_MoveList_Dribble.txt`, `_Tackle.txt`, `_Shot.txt`, `_AirShot.txt`,
  `_Saving.txt`, `_MiracleSaving.txt` — liste complete dei nomi mossa reali
  (431 mosse totali, dal tuo database save editor), una per riga.
- `CT2_MoveList_Super.txt` — catalogo Super Move con nome, tipo e ID
  (vedi nota sotto, SuperType non ancora confermato).
- `CT2_CustomPlayerMoves.txt` — il file che **tu** modifichi.

**Come si usa**: apri `CT2_CustomPlayerMoves.txt`, scrivi il nome esatto
(copia/incolla dalle liste `CT2_MoveList_*.txt`) accanto al campo che vuoi
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
1. Apri `CT2_MoveList_Super.txt` — contiene nome e ID per ogni mossa, ma
   non ancora il numero `SuperType` corretto.
2. Nel gioco, apri la schermata del giocatore Custom e nota la Super Move
   attualmente equipaggiata.
3. In `CT2_CustomPlayerMoves.txt`, prova `SuperType=0` con l'ID di una mossa
   che credi essere di quel tipo (es. un ID dalla lista `SHOOT`), salva,
   guarda se il gioco mostra quella mossa o un'altra/niente.
4. Ripeti con 1,2,3,4,5,7,8. Mandami quale numero mostra quale categoria —
   anche solo 2-3 conferme bastano per dedurre tutta la mappatura.

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
