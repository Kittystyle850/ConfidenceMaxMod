# CT2CheatDLL

## v1.1 — fix trampolino vicino + log onesto
La DLL viene caricata da Windows molto lontano dal modulo del gioco in
memoria (oltre i 2GB coperti da un `jmp` diretto x64). Finché il bug era
presente, ogni hook falliva silenziosamente e il log mentiva dicendo
"installato" comunque. Ora: se il bersaglio è troppo lontano, viene allocato
un piccolo trampolino entro i 2GB dal punto di hook (stesso trucco usato
dalla tua CT table con i suoi `jmp qword ptr [...]`), e il log riporta
`[+]`/`[!]` solo quando l'hook è *davvero* installato.

DLL x64 per **Captain Tsubasa 2: World Fighters** (offline), porting standalone
di 6 funzioni prese dalle tue due Cheat Table (Crack + Steam v1.3) in un unico
hook manuale (no Cheat Engine necessario in gioco). I pattern AOB sono
identici tra le due versioni (stesso `CaptainTsubasa2WF-Win64-Shipping.exe`,
cambia solo la parte DRM), quindi questa build funziona su entrambe.

## Trucchi inclusi (hotkey)
| Tasto | Effetto |
|---|---|
| F1 | Normal Tackle: Instant Maximum Charge |
| F2 | Super Dribble: tiene il chain count attivo e azzera il cooldown |
| F3 | Always PERFECT Dribble Grade |
| F4 | Instant Maximum-Charge Normal Shot |
| F5 | CHAIN MAX (condiviso, entrambe le squadre) |
| F6 | Unique Technique limit 2→4 (patch statica, toggle) |
| F9 | Scarica la DLL |
| ESC | Ferma il loop hotkey (gli hook restano attivi) |

Log: `CT2CheatDLL_log.txt` accanto all'exe del gioco.

## Semplificazione rispetto alla CT table originale
Le tabelle originali instradano ogni effetto attraverso un resolver di scope
("squadra alleata / nemica / giocatore controllato / giocatore in editing",
`ct2_should_lock`), che a sua volta dipende da una dozzina di altri hook
condivisi (indice pad, cache squadra home/away, classificazione controller)
usati anche da funzionalità che NON hai chiesto (stamina, HP portiere, super
mosse, miracle move...).

Per tenere la DLL autonoma e non rischiare crash per hook mancanti che altre
feature si aspettano, ho **tolto il resolver di scope**: ogni cheat qui è un
semplice ON/OFF globale (si applica a entrambe le squadre). La logica
dell'effetto vero e proprio (offset di struct, formula del cap CHAIN,
branching) è invece portata 1:1 dagli script della tua tabella.

Idem per CHAIN MAX: l'originale forza il livello ad ogni Tick *e* blocca i
decrementi nel setter ufficiale. Qui ho tenuto solo l'hook sul setter
ufficiale (si attiva ad ogni evento di gioco che aggiorna il CHAIN, che in
partita è comunque frequente) — niente hook aggiuntivo sul call-site del
Tick, per evitare una dipendenza in più senza reale necessità.

Se vuoi lo scope per-squadra fedele all'originale o il forcing per-frame del
CHAIN, dimmelo e li aggiungo sopra questa base.

## Build
Stessa pipeline di `ConfidenceMaxMod`: CMake + MASM (`ml64`, incluso nei
runner `windows-latest` di GitHub Actions) via `.github/workflows/build.yml`.
Nessuna dipendenza esterna, nessun framework di hooking: AOB-scan e jmp-hook
scritti a mano in `main.cpp` + codecave in `hooks.asm`.

`main.cpp` è stato validato con una cross-compilazione mingw-w64 (sintassi
C++ corretta); `ml64` può essere verificato solo sul runner Windows reale
della Action, dove verrà compilato al prossimo push.
