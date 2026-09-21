// ============================================================
//  MindCopTrainer v1.1  -  Freeze Timer per MindCop.exe (x64)
//  ------------------------------------------------------------
//  Hotkey:
//    F1  = congela / sblocca il Timer (beep acuto / grave)
//    F2  = unload DLL  (utile per ri-iniettare senza riavviare)
//    ESC = esce dal loop (la DLL resta caricata ma inerte)
//
//  Log: MindCopTrainer_log.txt (accanto all'EXE del gioco)
// ============================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <ctime>

// ---------- Info ----------
static constexpr const char* TRAINER_NAME = "MindCopTrainer";
static constexpr const char* TRAINER_VER  = "1.1";

// ---------- Catena di puntatori (dalla cheat table) ----------
// Ordine: dal fondo della struttura verso la cima.
static constexpr uintptr_t BASE_OFFSET = 0x00BDB8D0;
static constexpr uintptr_t OFFSETS[]   = { 0x360, 0x2B0, 0x48, 0x10, 0x3E0, 0x124 };
static constexpr int       OFFSETS_COUNT =
    static_cast<int>(sizeof(OFFSETS) / sizeof(OFFSETS[0]));

// ---------- Hotkey ----------
// NB: VK_F1, VK_F2, VK_ESCAPE sono macro di <windows.h>.
// Non ridichiararle con lo stesso nome, altrimenti il preprocessore
// le espande e produce errori C2059. Usiamo prefisso "KEY_".
static constexpr int KEY_F1  = VK_F1;      // 0x70
static constexpr int KEY_F2  = VK_F2;      // 0x71
static constexpr int KEY_ESC = VK_ESCAPE;  // 0x1B

// ---------- Stato globale ----------
static uintptr_t g_moduleBase = 0;
static HMODULE   g_hSelf      = nullptr;

// ============================================================
//  Log
// ============================================================
static void WriteLog(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "MindCopTrainer_log.txt", "a") != 0 || !f) return;

    // Timestamp
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
    std::fprintf(f, "[%s] ", ts);

    // Messaggio
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);

    std::fputc('\n', f);
    std::fclose(f);
}

// ============================================================
//  Accesso sicuro alla memoria (SEH)
//  Evita crash se un puntatore è invalido (es. in menu / loading).
// ============================================================
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

// ============================================================
//  Risoluzione catena di puntatori
// ============================================================
static uintptr_t ResolveTimerAddress() {
    uintptr_t addr = g_moduleBase + BASE_OFFSET;
    for (int i = 0; i < OFFSETS_COUNT; ++i) {
        uintptr_t ptr = 0;
        if (!SafeReadPtr(addr, ptr) || ptr == 0) return 0;
        addr = ptr + OFFSETS[i];
    }
    return addr;
}

// ============================================================
//  Thread principale
// ============================================================
static DWORD WINAPI MainThread(LPVOID) {
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!g_moduleBase) {
        WriteLog("[!] GetModuleHandleA ha restituito NULL, esco.");
        return 1;
    }

    WriteLog("[i] %s v%s avviato. Module base = 0x%llX",
             TRAINER_NAME, TRAINER_VER,
             static_cast<unsigned long long>(g_moduleBase));

    bool      frozen      = false;
    float     frozenValue = 0.0f;
    uintptr_t timerAddr   = 0;
    bool      lastF1      = false;
    bool      lastF2      = false;

    while (true) {
        // ---- ESC: esce dal loop ----
        if (GetAsyncKeyState(KEY_ESC) & 0x8000) {
            WriteLog("[i] ESC premuto, esco dal loop.");
            break;
        }

        // ---- F2: unload DLL ----
        bool f2 = (GetAsyncKeyState(KEY_F2) & 0x8000) != 0;
        if (f2 && !lastF2) {
            WriteLog("[i] F2 premuto, unload DLL.");
            Beep(400, 150);
            FreeLibraryAndExitThread(g_hSelf, 0);
            // non si torna qui
        }
        lastF2 = f2;

        // ---- F1: toggle freeze ----
        bool f1 = (GetAsyncKeyState(KEY_F1) & 0x8000) != 0;
        if (f1 && !lastF1) {
            if (!frozen) {
                // Risolvi e leggi il valore attuale
                uintptr_t addr = ResolveTimerAddress();
                float v = 0.0f;
                if (addr && SafeReadFloat(addr, v)) {
                    timerAddr   = addr;
                    frozenValue = v;
                    frozen      = true;
                    Beep(1200, 180);
                    WriteLog("[+] FREEZE  @ 0x%llX  valore=%.3f",
                             static_cast<unsigned long long>(addr), v);
                } else {
                    Beep(300, 400);
                    WriteLog("[!] Catena di puntatori non risolta.");
                }
            } else {
                frozen = false;
                Beep(600, 180);
                WriteLog("[+] UNFREEZE");
            }
        }
        lastF1 = f1;

        // ---- Riscrittura del valore congelato ----
        // Usa la cache; ri-risolve solo se la scrittura fallisce
        // (es. il gioco ha ricreato l'oggetto dopo un cambio livello).
        if (frozen) {
            if (timerAddr == 0 || !SafeWriteFloat(timerAddr, frozenValue)) {
                uintptr_t fresh = ResolveTimerAddress();
                if (fresh && SafeWriteFloat(fresh, frozenValue)) {
                    timerAddr = fresh;
                }
                // altrimenti aspetta il prossimo tick
            }
        }

        Sleep(8); // ~120 Hz
    }

    WriteLog("[i] Thread terminato.");
    return 0;
}

// ============================================================
//  DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
