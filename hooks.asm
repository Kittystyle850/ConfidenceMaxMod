; ============================================================
;  hooks.asm  -  CT2CheatDLL codecave (x64 MASM)
;  ------------------------------------------------------------
;  Trimmed build: only CHAIN MAX remains hooked here.
;  (Unique Technique limit is a plain static byte patch applied
;  directly from main.cpp, no codecave needed for it.)
;
;  Port of ChainSystem::SetLevel entry hook from the uploaded CT
;  tables. g_flag_chain defaults to 1 (ON) so the effect is active
;  the moment the hook installs, no hotkey involved.
; ============================================================

.data

PUBLIC g_flag_chain
g_flag_chain db 1                 ; ON by default, no toggle

PUBLIC g_ret_chain_level_set
g_ret_chain_level_set dq 0        ; filled by main.cpp after the AOB is found

.code

; ============================================================
; CHAIN MAX  -  ChainSystem::SetLevel entry hook
;     Patch site: "48 89 5C 24 18 48 89 6C 24 20" (10 bytes)
;                 = mov [rsp+18],rbx ; mov [rsp+20],rbp
;     rcx = ChainSystem* (this), edx = requested level (in/out)
;     Cap formula ported verbatim from the source table's
;     ct2_chain_get_max:
;       cap = [rcx+30]
;       if [rcx+40]==0               -> cap -= 1
;       elif byte[[rcx+40]+0xC99]==0 -> cap unchanged
;       else                         -> cap -= 1
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
cls_done:
    jmp     qword ptr [g_ret_chain_level_set]
ct2_chain_level_set_hook ENDP

END
