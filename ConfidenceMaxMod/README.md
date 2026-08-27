# ConfidenceMaxMod

Mod standalone (DLL) che imposta sempre al massimo la Confidence Gauge, convertita da uno script Cheat Engine.

## Come usarlo su GitHub

1. Crea un nuovo repository su GitHub (vuoto, senza README).
2. Carica **tutto il contenuto** di questa cartella (compresa la cartella `.github`) nel repository, mantenendo la stessa struttura.
3. Vai nella tab **Actions** del repository: la build partirà automaticamente al primo push.
4. A build completata, apri l'esecuzione del workflow "Build Mod DLL" e scarica l'artifact `ConfidenceMaxMod`: dentro trovi `ConfidenceMaxMod.dll`.

## Come installare la mod (per l'utente finale)

Rinomina la DLL in `version.dll` (DLL proxying) e posizionala nella cartella principale del gioco, accanto a `PWAAT_Data`. Il gioco la caricherà automaticamente all'avvio, senza bisogno di injector esterni.
