; stubs.asm  (MASM, x64 - assembled by ml64 via CMake ASM_MASM)
;
; Code caves ported 1:1 from SeiKur0's Shadow of War CT (v21).
; Each stub is entered through a 5/6-byte "jmp" patched into ShadowOfWar.exe
; (game -> near trampoline -> stub) and leaves through "jmp [ret_xxx]",
; the return slot is filled by the C++ side with (hook site + patch length).
; Registers/flags are exactly what the original CT cave expects.

OPTION CASEMAP:NONE

PUBLIC g_rarityLegendary
PUBLIC g_dropItem
PUBLIC g_lootNemGear
PUBLIC ret_rarity
PUBLIC ret_intel
PUBLIC ret_drop
PUBLIC SowRarityStub
PUBLIC SowIntelStub
PUBLIC SowDropStub

.data
ALIGN 8
g_rarityLegendary db 1, 7 dup (0)   ; 1 = legendary, 0 = epic
g_dropItem        dq 0              ; NemesisGear entry to force (0 = untouched)
g_lootNemGear     dq 0              ; list base of "Faction/Loot/NemesisGear"
ret_rarity        dq 0
ret_intel         dq 0
ret_drop          dq 0

.code

; ---------------------------------------------------------------------------
; Uruk spawn rarity   (CT: inj_creation, hook @ AOB-0x1E, overwrites
;                      57 / 48 83 EC 60 = push rdi; sub rsp,60)
; Replaces the function for the two rarity-table keys, otherwise runs the
; stolen prologue and goes back.
; ---------------------------------------------------------------------------
SowRarityStub PROC
    cmp     dword ptr [rdx+4], 0E6FC03C6h
    je      rarity_key_a
    cmp     dword ptr [rdx+4], 07AEAD12h
    je      rarity_key_b

    push    rdi                         ; stolen instructions
    sub     rsp, 60h
    jmp     qword ptr [ret_rarity]

rarity_key_a:
    mov     rax, [rdx+10h]
    cmp     byte ptr [g_rarityLegendary], 0
    je      rarity_key_a_epic
    mov     rax, [rax+30h]
    ret
rarity_key_a_epic:
    mov     rax, [rax+20h]
    ret

rarity_key_b:
    mov     rax, [rdx+10h]
    cmp     byte ptr [g_rarityLegendary], 0
    je      rarity_key_b_epic
    mov     rax, [rax+28h]
    ret
rarity_key_b_epic:
    mov     rax, [rax+18h]
    ret
SowRarityStub ENDP

; ---------------------------------------------------------------------------
; Get all Uruk intel  (CT: inj_urukintel, hook @ AOB+0xB, overwrites
;                      BA 03 00 00 00 = mov edx,3)
; ---------------------------------------------------------------------------
SowIntelStub PROC
    mov     edx, [rcx]
    or      edx, 0FBh
    mov     [rcx], edx
    mov     edx, [rcx+0Ah]
    and     edx, 0FFFFFFFEh
    or      edx, 2
    mov     [rcx+0Ah], edx
    mov     edx, 3                      ; stolen instruction
    jmp     qword ptr [ret_intel]
SowIntelStub ENDP

; ---------------------------------------------------------------------------
; Epic/Legendary Uruks drop a chosen item  (CT: inj_drop, hook @ AOB+0xD,
; overwrites 48 8B D8 / 48 85 C0 = mov rbx,rax; test rax,rax)
; rbx is dead at this point (overwritten right after), so it is free scratch.
; Flags from the final "test rax,rax" feed the original "je" that follows.
; ---------------------------------------------------------------------------
SowDropStub PROC
    test    rax, rax
    jz      drop_done
    mov     rbx, [g_lootNemGear]
    test    rbx, rbx
    jz      drop_done
    cmp     [rax+18h], rbx              ; is this a NemesisGear loot roll?
    jne     drop_done
    mov     rbx, [g_dropItem]
    test    rbx, rbx
    jz      drop_done
    mov     rax, rbx                    ; force our item
drop_done:
    mov     rbx, rax                    ; stolen instructions
    test    rax, rax
    jmp     qword ptr [ret_drop]
SowDropStub ENDP

END
