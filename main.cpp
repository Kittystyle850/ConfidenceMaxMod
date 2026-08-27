#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>

// Indirizzi per il salto di ritorno
uintptr_t returnAddress = 0;

// La nostra funzione "trampolino" (equivalente al tuo GlobalWork_newmem)
__declspec(naked) void ConfidenceHook() {
    __asm {
        // Salva i flag della CPU per sicurezza
        pushfd 

        // cmp byte ptr [eax+3c], #4 come nel tuo script originale
        cmp byte ptr [eax + 0x3C], 0x04
        je SkipCheat
        
        // Imposta la Confidence Gauge al massimo
        mov byte ptr [eax + 0xCB], 0x05
        mov word ptr [eax + 0xA2], 0x0080
        mov word ptr [eax + 0xA4], 0x0080

    SkipCheat:
        // Ripristina i flag
        popfd

        // Esegue il codice originale (6 byte)
        // 8B 80 C4 00 00 00 -> mov eax,[eax+000000C4]
        mov eax, dword ptr [eax + 0xC4]

        // Salta indietro al gioco (equivalente a jmp GlobalWork_returnhere)
        jmp [returnAddress]
    }
}

// Funzione basilare per scansionare la memoria alla ricerca dell'AOB
uintptr_t FindPattern(const char* pattern, const char* mask) {
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) return 0;

    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)hModule;
    IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)((BYTE*)hModule + dosHeader->e_lfanew);
    
    BYTE* base = (BYTE*)hModule;
    DWORD size = ntHeaders->OptionalHeader.SizeOfImage;
    DWORD patternLength = (DWORD)strlen(mask);

    for (DWORD i = 0; i < size - patternLength; i++) {
        bool found = true;
        for (DWORD j = 0; j < patternLength; j++) {
            if (mask[j] != '?' && pattern[j] != base[i + j]) {
                found = false;
                break;
            }
        }
        if (found) return (uintptr_t)(base + i);
    }
    return 0;
}

void WriteLog(const std::string& msg) {
    FILE* f = nullptr;
    fopen_s(&f, "ConfidenceMaxMod_log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", msg.c_str());
        fclose(f);
    }
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    // AOB esatto, identico a quello dello script Cheat Engine (nessun wildcard)
    const char* aob = "\x8B\x80\xC4\x00\x00\x00\x85\xC0\x39\x3F\x0F\xB6\x47\x5F";
    const char* mask = "xxxxxxxxxxxxxx"; // 14 byte tutti esatti, come aobscan() in CE

    uintptr_t hookAddress = FindPattern(aob, mask);

    char buf[128];
    sprintf_s(buf, "AOB trovato a: 0x%08X", (unsigned int)hookAddress);
    WriteLog(hookAddress ? buf : "AOB NON TROVATO - pattern non presente in memoria");

    if (hookAddress) {
        // Calcola l'indirizzo di ritorno (hookAddress + 6 byte istruzione originale)
        returnAddress = hookAddress + 6;

        // Rimuove la protezione della memoria per poter scrivere l'hook
        DWORD oldProtect;
        VirtualProtect((void*)hookAddress, 6, PAGE_EXECUTE_READWRITE, &oldProtect);

        // Scrive l'istruzione JMP (0xE9) e calcola l'offset relativo
        *(BYTE*)hookAddress = 0xE9;
        *(uintptr_t*)(hookAddress + 1) = (uintptr_t)ConfidenceHook - hookAddress - 5;
        
        // Inserisce un NOP (0x90) per il 6° byte avanzato (il JMP ne occupa 5)
        *(BYTE*)(hookAddress + 5) = 0x90;

        // Ripristina la protezione della memoria
        VirtualProtect((void*)hookAddress, 6, oldProtect, &oldProtect);

        WriteLog("Hook JMP scritto correttamente.");
    }

    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}
