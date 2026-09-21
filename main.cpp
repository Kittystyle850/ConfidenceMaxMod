// ============================================================
//  MindCopTrainer - Freeze Timer per MindCop.exe (x64)
//  - F1  = congela/sblocca il Timer
//  - ESC = scarica il thread (la DLL resta caricata)
// ============================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>

// ----- Catena di puntatori (identica alla cheat table / Python) -----
static constexpr uintptr_t BASE_OFFSET = 0x00BDB8D0;
static constexpr uintptr_t OFFSETS[] = { 0x360, 0x2B0, 0x48, 0x10, 0x3E0, 0x124 };
static constexpr int       OFFSETS_COUNT =
    sizeof(OFFSETS) / sizeof(OFFSETS[0]);

static constexpr int VK_F1  = 0x70;
static constexpr int VK_ESC = 0x1B;

static uintptr_t g_moduleBase = 0;

// ----- Accesso sicuro alla memoria (evita crash su puntatori invalidi) -----
static bool SafeReadPtr(uintptr_t addr, uintptr_t& out) {
    __try { out = *reinterpret_cast<const uintptr_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeReadFloat(uintptr_t addr, float& out) {
    __try { out = *reinterpret_cast<const float*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWriteFloat(uintptr_t addr, float v) {
    __try { *reinterpret_cast<float*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ----- Risoluzione catena [MindCop.exe+0xBDB8D0] + offsets -----
static uintptr_t ResolveTimerAddress() {
    uintptr_t addr = g_moduleBase + BASE_OFFSET;
    for (int i = 0; i < OFFSETS_COUNT; ++i) {
        uintptr_t ptr = 0;
        if (!SafeReadPtr(addr, ptr) || ptr == 0) return 0;
        addr = ptr + OFFSETS[i];
    }
    return addr;
}

// ----- Log opzionale su file -----
static void WriteLog(const char* msg) {
    FILE* f = nullptr;
    fopen_s(&f, "MindCopTrainer_log.txt", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ----- Thread principale -----
static DWORD WINAPI MainThread(LPVOID) {
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!g_moduleBase) return 1;

    char buf[160];
    sprintf_s(buf, "[+] module base = 0x%llX",
              static_cast<unsigned long long>(g_moduleBase));
    WriteLog(buf);

    bool      frozen      = false;
    float     frozenValue = 0.0f;
    uintptr_t timerAddr   = 0;
    bool      lastKey     = false;

    while (true) {
        // Uscita dal loop (la DLL resta caricata, ma il thread termina)
        if (GetAsyncKeyState(VK_ESC) & 0x8000) break;

        // Edge detection su F1
        bool key = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (key && !lastKey) {
            if (!frozen) {
                uintptr_t addr = ResolveTimerAddress();
                float v = 0.0f;
                if (addr && SafeReadFloat(addr, v)) {
                    timerAddr   = addr;
                    frozenValue = v;
                    frozen      = true;
                    Beep(1200, 180);
                    sprintf_s(buf, "[+] FREEZE @ 0x%llX valore=%.3f",
                              static_cast<unsigned long long>(addr), v);
                    WriteLog(buf);
                } else {
                    Beep(300, 400);
                    WriteLog("[!] catena non risolta");
                }
            } else {
                frozen = false;
                Beep(600, 180);
                WriteLog("[+] UNFREEZE");
            }
        }
        lastKey = key;

        // Riscrive il valore congelato. Ri-risolve la catena ad ogni tick
        // così se il gioco ricrea l'oggetto (cambio livello) non restiamo
        // agganciati a memoria vecchia.
        if (frozen) {
            uintptr_t addr = ResolveTimerAddress();
            if (addr) timerAddr = addr;
            if (timerAddr) SafeWriteFloat(timerAddr, frozenValue);
        }

        Sleep(10); // ~100 Hz, come lo script Python
    }

    WriteLog("[i] Thread terminato.");
    return 0;
}

// ----- DllMain -----
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
