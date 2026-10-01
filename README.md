# CT2CheatDLL

DLL x64 per **Captain Tsubasa 2: World Fighters** (offline). Versione
minimale: solo 2 effetti, entrambi **attivi di default all'avvio**, nessun
tasto, nessun suono.

## Effetti
- **CHAIN MAX** — il CHAIN (condiviso, vale per entrambe le squadre) resta
  al massimo dinamico consentito dalla modalità.
- **Unique Technique limit 2 → 4** — patch statica, 4 slot tecniche uniche
  normali invece di 2.

Funziona sia sulla build Crack che su quella Steam v1.3: stesso
`CaptainTsubasa2WF-Win64-Shipping.exe`, stesso codice di gameplay, cambia
solo la parte DRM.

## Log
`CT2CheatDLL_log.txt`, creato accanto all'exe del gioco. Righe `[+]` = ok,
`[!]` = qualcosa non trovato/non installato. `F9` scarica la DLL, `ESC` ferma
il thread di controllo (gli effetti restano comunque attivi in memoria).

## Changelog
- **v2.0** — rimossi Instant Tackle Charge, Super Dribble lock, Always
  PERFECT Dribble, Instant Shot Charge, relativi hotkey (F1-F5) e feedback
  audio. Restano solo CHAIN MAX e Unique Technique Limit, entrambi forzati
  ON all'avvio senza alcuna interazione.
- **v1.1** — fix trampolino vicino: la DLL può caricarsi molto lontano dal
  modulo del gioco in memoria (oltre i 2GB coperti da un `jmp` diretto x64).
  Prima il bug faceva fallire silenziosamente ogni hook mentre il log diceva
  "installato" comunque. Ora, se il bersaglio è troppo lontano, viene
  allocato un piccolo trampolino entro i 2GB dal punto di hook (stesso
  trucco usato dalla CT table originale con i suoi `jmp qword ptr [...]`),
  e il log riporta `[+]`/`[!]` solo quando l'hook è davvero installato.

## Build
CMake + MASM (`ml64`, incluso nei runner `windows-latest` di GitHub
Actions) via `.github/workflows/build.yml`. Nessuna dipendenza esterna,
nessun framework di hooking: AOB-scan e jmp-hook scritti a mano in
`main.cpp` + un codecave in `hooks.asm`.
