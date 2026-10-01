; ============================================================
;  hooks.asm  -  CT2CheatDLL codecaves (x64 MASM)
;  ------------------------------------------------------------
;  Every routine here is a 1:1, simplified port of the AOB/asm
;  logic found in the uploaded Cheat Engine tables
;  (CAPTAIN_TSUBASA_2_WORLD_FIGHTERS_OFFLINE_v1_3_ENGLISH_*.CT).
;  Simplification vs the original CT scripts: the original tables
;  gate every effect through an "ally/enemy/current/edit-player"
;  scope resolver (ct2_should_lock). That resolver depends on a
;  dozen other hooks (pad index, home/away slot cache, controller
;  classification) that are NOT needed for the 6 requested cheats
;  on their own, so it has been dropped: every flag below is
;  simply "ON = applies to both sides" / "OFF = native behaviour".
;  The byte patterns, struct offsets and branch logic for the
;  actual effect are kept faithful to the source table.
; ============================================================

.data

; ---- toggle flags (BYTE, 0/1), flipped by hotkeys in main.cpp ----
PUBLIC g_flag_shot
PUBLIC g_flag_chain
PUBLIC g_flag_dribble
PUBLIC g_flag_sdribble
PUBLIC g_flag_ntcharge

g_flag_shot     db 0
g_flag_chain    db 0
g_flag_dribble  db 0
g_flag_sdribble db 0
g_flag_ntcharge db 0

; ---- jump-back targets, filled by main.cpp right after each AOB
;      is resolved (= found_addr + patched_length) ----
PUBLIC g_ret_shot_tick
PUBLIC g_ret_shot_release
PUBLIC g_ret_sdribble_end
PUBLIC g_ret_sdribble_direct
PUBLIC g_ret_sdribble_cooldown
PUBLIC g_ret_ntcharge_tick
PUBLIC g_ret_ntcharge_release
PUBLIC g_ret_chain_level_set

g_ret_shot_tick        dq 0
g_ret_shot_release     dq 0
g_ret_sdribble_end     dq 0
g_ret_sdribble_direct  dq 0
g_ret_sdribble_cooldown dq 0
g_ret_ntcharge_tick     dq 0
g_ret_ntcharge_release  dq 0
g_ret_chain_level_set   dq 0

; ---- hit counters, purely diagnostic (readable from the log) ----
PUBLIC g_hits_shot
PUBLIC g_hits_chain
PUBLIC g_hits_dribble
PUBLIC g_hits_sdribble
PUBLIC g_hits_ntcharge
g_hits_shot     dd 0
g_hits_chain    dd 0
g_hits_dribble  dd 0
g_hits_sdribble dd 0
g_hits_ntcharge dd 0

.code

; ============================================================
; [1] Instant Maximum-Charge Normal Shot
;     Patch site A: "F3 0F 10 80 20 0A 00 00" = movss xmm0,[rax+0A20]
;                    (normal-shot charge TICK, 8 bytes)
;     Patch site B: identical 8 bytes, different function
;                    (normal-shot RELEASE, writes into [rsi+754])
; ============================================================

PUBLIC ct2_shot_tick_hook
ct2_shot_tick_hook PROC
    movss   xmm0, dword ptr [rax+0A20h]      ; re-execute overwritten instr
    cmp     byte ptr [g_flag_shot], 0
    je      stick_done
    movss   xmm7, xmm0                        ; force full charge this tick
    inc     dword ptr [g_hits_shot]
stick_done:
    jmp     qword ptr [g_ret_shot_tick]
ct2_shot_tick_hook ENDP

PUBLIC ct2_shot_release_hook
ct2_shot_release_hook PROC
    movss   xmm0, dword ptr [rax+0A20h]      ; re-execute overwritten instr
    cmp     byte ptr [g_flag_shot], 0
    je      srel_done
    movss   dword ptr [rsi+754h], xmm0        ; commit max charge as current
    inc     dword ptr [g_hits_shot]
srel_done:
    jmp     qword ptr [g_ret_shot_release]
ct2_shot_release_hook ENDP

; ============================================================
; [2] CHAIN MAX  -  ChainSystem::SetLevel entry hook
;     Patch site: "48 89 5C 24 18 48 89 6C 24 20" (10 bytes)
;                 = mov [rsp+18],rbx ; mov [rsp+20],rbp
;     rcx = ChainSystem* (this), edx = requested level (in/out)
;     Cap formula ported verbatim from ct2_chain_get_max:
;       cap = [rcx+30]
;       if [rcx+40]==0            -> cap -= 1
;       elif byte[[rcx+40]+0xC99]==0 -> cap unchanged
;       else                      -> cap -= 1
; ============================================================

PUBLIC ct2_chain_level_set_hook
ct2_chain_level_set_hook PROC
    mov     qword ptr [rsp+18h], rbx          ; re-execute overwritten instrs
    mov     qword ptr [rsp+20h], rbp

    cmp     byte ptr [g_flag_chain], 0
    je      cls_done
    test    rcx, rcx
    je      cls_done
    mov     eax, dword ptr [rcx+30h]
    test    eax, eax
    jle     cls_done
    cmp     eax, 40h
    ja      cls_done

    mov     r11, qword ptr [rcx+40h]
    test    r11, r11
    je      cls_no_extra
    cmp     byte ptr [r11+0C99h], 0
    je      cls_gotcap
cls_no_extra:
    dec     eax
cls_gotcap:
    cmp     edx, eax
    je      cls_done                           ; already at cap
    mov     edx, eax
    inc     dword ptr [g_hits_chain]
cls_done:
    jmp     qword ptr [g_ret_chain_level_set]
ct2_chain_level_set_hook ENDP

; ============================================================
; [3] Always PERFECT Dribble Grade  -  full leaf-function replace
;     Original function (7 bytes @ entry, everything after is
;     reproduced here so we just RET instead of trampolining back):
;         movzx eax, byte ptr [rcx+35B1]
;         mov   rdx, rcx
;         cmp   al, 21h
;         jne   +6
;         mov   eax, -4
;         ret
;     rcx = AChara*. Native: byte(-4)=full-power special (untouched),
;     0/1/2 = normal/good/perfect-ish raw grade -> forced to 0 (PERFECT).
; ============================================================

PUBLIC ct2_duel_dribble_hook
ct2_duel_dribble_hook PROC
    movzx   eax, byte ptr [rcx+35B1h]
    mov     rdx, rcx
    cmp     al, 21h
    jne     dd_keep
    mov     eax, 0FFFFFFFCh                    ; -4, FULL_POWER_DRIBBLE
    jmp     dd_check_flag
dd_keep:
dd_check_flag:
    cmp     eax, 2
    ja      dd_ret                             ; special case, leave as-is
    cmp     byte ptr [g_flag_dribble], 0
    je      dd_ret
    ; actor-state sanity checks ported from the table (self-contained,
    ; no scope resolver needed): skip special-flagged actors and
    ; actors without a real normal-dribble config loaded.
    test    dword ptr [rcx+8], 40000000h
    jne     dd_ret
    cmp     qword ptr [rcx+1238h], 0
    je      dd_ret
    xor     eax, eax                            ; force PERFECT grade
    inc     dword ptr [g_hits_dribble]
dd_ret:
    ret
ct2_duel_dribble_hook ENDP

; ============================================================
; [4] Super Dribble: keep active chain count / clear cooldown
;     Three independent patch sites sharing two flag bytes.
; ============================================================

; site A: "89 83 B0 3B 00 00" = mov [rbx+3BB0],eax   (end-of-move count store)
PUBLIC ct2_sdribble_end_hook
ct2_sdribble_end_hook PROC
    cmp     byte ptr [g_flag_sdribble], 0
    jne     se_skip_store                        ; cheat ON: drop the decrement
    mov     dword ptr [rbx+3BB0h], eax            ; cheat OFF: native store
    jmp     se_done
se_skip_store:
    inc     dword ptr [g_hits_sdribble]
se_done:
    jmp     qword ptr [g_ret_sdribble_end]
ct2_sdribble_end_hook ENDP

; site B: "FF 89 B0 3B 00 00" = dec dword ptr [rcx+3BB0]   (direct count decrement)
PUBLIC ct2_sdribble_direct_hook
ct2_sdribble_direct_hook PROC
    cmp     byte ptr [g_flag_sdribble], 0
    jne     sdir_skip_dec
    dec     dword ptr [rcx+3BB0h]
    jmp     sdir_done
sdir_skip_dec:
    inc     dword ptr [g_hits_sdribble]
sdir_done:
    jmp     qword ptr [g_ret_sdribble_direct]
ct2_sdribble_direct_hook ENDP

; site C: "F3 0F 10 83 EC 3B 00 00" = movss xmm0,[rbx+3BEC]  (cooldown read)
PUBLIC ct2_sdribble_cooldown_hook
ct2_sdribble_cooldown_hook PROC
    cmp     byte ptr [g_flag_sdribble], 0
    je      scd_native
    xorps   xmm0, xmm0
    movss   dword ptr [rbx+3BECh], xmm0           ; clear live cooldown
    inc     dword ptr [g_hits_sdribble]
    jmp     scd_done
scd_native:
    movss   xmm0, dword ptr [rbx+3BECh]
scd_done:
    jmp     qword ptr [g_ret_sdribble_cooldown]
ct2_sdribble_cooldown_hook ENDP

; ============================================================
; [5] Normal Tackle: Instant Max Charge
;     Patch site A: "F3 0F 5D 86 74 07 00 00" = minss xmm0,[rsi+774]  (tick)
;     Patch site B: "0F 2F 87 74 07 00 00"    = comiss xmm0,[rdi+774] (release)
; ============================================================

PUBLIC ct2_ntcharge_tick_hook
ct2_ntcharge_tick_hook PROC
    cmp     byte ptr [g_flag_ntcharge], 0
    jne     nt_skip_clamp                         ; cheat ON: keep xmm0 unclamped (=max)
    minss   xmm0, dword ptr [rsi+774h]
    jmp     nt_tick_done
nt_skip_clamp:
    inc     dword ptr [g_hits_ntcharge]
nt_tick_done:
    jmp     qword ptr [g_ret_ntcharge_tick]
ct2_ntcharge_tick_hook ENDP

PUBLIC ct2_ntcharge_release_hook
ct2_ntcharge_release_hook PROC
    cmp     byte ptr [g_flag_ntcharge], 0
    je      ntr_native
    movss   dword ptr [rdi+774h], xmm0             ; commit max charge
    inc     dword ptr [g_hits_ntcharge]
ntr_native:
    comiss  xmm0, dword ptr [rdi+774h]              ; re-execute overwritten instr
    jmp     qword ptr [g_ret_ntcharge_release]
ct2_ntcharge_release_hook ENDP

END
