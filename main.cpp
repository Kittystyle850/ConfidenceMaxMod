// ============================================================
//  CT2CheatDLL v1.0  -  Captain Tsubasa 2: World Fighters (x64)
//  ------------------------------------------------------------
//  Standalone port of 6 entries from the uploaded Cheat Engine
//  tables (CAPTAIN_TSUBASA_2_WORLD_FIGHTERS_OFFLINE_v1_3_*.CT).
//  Works unmodified against BOTH uploaded targets (Crack + Steam):
//  the gameplay code section is byte-identical between the two,
//  only unrelated save/DRM-adjacent AOBs differ in the source
//  tables, none of which are used here.
//
//  Hotkeys (edge-triggered, polled @ ~120 Hz):
//    F1 = Normal Tackle: Instant Max Charge
//    F2 = Super Dribble: keep chain count / no cooldown
//    F3 = Always PERFECT Dribble Grade
//    F4 = Instant Maximum-Charge Normal Shot
//    F5 = Keep CHAIN at Maximum (shared, both teams)
//    F6 = Unique Technique limit 2 -> 4 (one-shot static patch,
//         press again to revert)
//    F9  = unload DLL
//    ESC = stop the hotkey loop (DLL stays mapped, hooks stay live)
//
//  Log: CT2CheatDLL_log.txt (next to the game EXE)
// ============================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// ---- symbols defined in hooks.asm ----
extern "C" {
    extern uint8_t g_flag_shot;
    extern uint8_t g_flag_chain;
    extern uint8_t g_flag_dribble;
    extern uint8_t g_flag_sdribble;
    extern uint8_t g_flag_ntcharge;

    extern uint64_t g_ret_shot_tick;
    extern uint64_t g_ret_shot_release;
    extern uint64_t g_ret_sdribble_end;
    extern uint64_t g_ret_sdribble_direct;
    extern uint64_t g_ret_sdribble_cooldown;
    extern uint64_t g_ret_ntcharge_tick;
    extern uint64_t g_ret_ntcharge_release;
    extern uint64_t g_ret_chain_level_set;

    extern uint32_t g_hits_shot;
    extern uint32_t g_hits_chain;
    extern uint32_t g_hits_dribble;
    extern uint32_t g_hits_sdribble;
    extern uint32_t g_hits_ntcharge;

    void ct2_shot_tick_hook();
    void ct2_shot_release_hook();
    void ct2_chain_level_set_hook();
    void ct2_duel_dribble_hook();
    void ct2_sdribble_end_hook();
    void ct2_sdribble_direct_hook();
    void ct2_sdribble_cooldown_hook();
    void ct2_ntcharge_tick_hook();
    void ct2_ntcharge_release_hook();
}

static constexpr const char* TRAINER_NAME = "CT2CheatDLL";
static constexpr const char* TRAINER_VER  = "1.0";

static HMODULE g_hSelf = nullptr;

// ============================================================
//  Log
// ============================================================
static void WriteLog(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "CT2CheatDLL_log.txt", "a") != 0 || !f) return;
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
    std::fprintf(f, "[%s] ", ts);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

// ============================================================
//  AOB scanner (pattern string: "F3 0F 10 80 20 0A 00 00" / "??" wildcard)
// ============================================================
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask; // true = must match
};

static Pattern ParsePattern(const char* str) {
    Pattern p;
    const char* s = str;
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?' ) {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            while (*s == '?') ++s;
        } else {
            unsigned v = 0;
            sscanf_s(s, "%2x", &v);
            p.bytes.push_back(static_cast<uint8_t>(v));
            p.mask.push_back(true);
            s += 2;
        }
    }
    return p;
}

static uintptr_t FindPattern(uintptr_t base, size_t size, const char* patStr) {
    Pattern pat = ParsePattern(patStr);
    if (pat.bytes.empty()) return 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(base);
    const size_t plen = pat.bytes.size();
    if (size < plen) return 0;
    for (size_t i = 0; i <= size - plen; ++i) {
        bool ok = true;
        for (size_t j = 0; j < plen; ++j) {
            if (pat.mask[j] && data[i + j] != pat.bytes[j]) { ok = false; break; }
        }
        if (ok) return base + i;
    }
    return 0;
}

// ============================================================
//  Hook writer: overwrite `patchLen` bytes at `addr` with
//  E9 rel32 -> target, padded with NOP. Returns addr+patchLen
//  (the "return address" for mid-function hooks).
// ============================================================
static uintptr_t WriteJmpHook(uintptr_t addr, void* target, int patchLen) {
    if (patchLen < 5) { WriteLog("[!] patchLen<5 @ 0x%llX", (unsigned long long)addr); return 0; }
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), patchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        WriteLog("[!] VirtualProtect failed @ 0x%llX (err=%lu)", (unsigned long long)addr, GetLastError());
        return 0;
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
    int64_t rel = reinterpret_cast<int64_t>(target) - (static_cast<int64_t>(addr) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        WriteLog("[!] codecave out of +-2GB range @ 0x%llX (target 0x%llX)",
                 (unsigned long long)addr, (unsigned long long)target);
        VirtualProtect(reinterpret_cast<LPVOID>(addr), patchLen, oldProt, &oldProt);
        return 0;
    }
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(rel);
    for (int i = 5; i < patchLen; ++i) p[i] = 0x90; // NOP padding
    VirtualProtect(reinterpret_cast<LPVOID>(addr), patchLen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), patchLen);
    return addr + patchLen;
}

static bool WriteBytesRaw(uintptr_t addr, const uint8_t* bytes, size_t len) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(reinterpret_cast<void*>(addr), bytes, len);
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), len);
    return true;
}

// ============================================================
//  AOB patterns (lifted verbatim from the uploaded .CT tables;
//  identical in both the Crack and Steam versions).
// ============================================================
static constexpr const char* PAT_SHOT_TICK      = "F3 0F 10 80 20 0A 00 00 F3 0F 5D C7 F3 0F 11 87 54 07 00 00";
static constexpr const char* PAT_SHOT_RELEASE   = "F3 0F 10 80 20 0A 00 00 0F 2F 86 54 07 00 00 0F 87 ?? ?? ?? ??";
static constexpr const char* PAT_CHAIN_LVLSET   = "48 89 5C 24 18 48 89 6C 24 20 56 57 41 57 48 83 EC 50 48 63 41 24";
static constexpr const char* PAT_DUEL_DRIBBLE   = "0F B6 81 B1 35 00 00 48 8B D1 3C 21 75 06 B8 FC FF FF FF C3";
static constexpr const char* PAT_SDRIBBLE_END   = "89 83 B0 3B 00 00 85 C0 7E 05 80 3A 00 74 72 0F B6 83 B0 35 00 00";
static constexpr const char* PAT_SDRIBBLE_DIR   = "FF 89 B0 3B 00 00 48 8B D9 83 B9 B0 3B 00 00 00 7E 09";
static constexpr const char* PAT_SDRIBBLE_CD    = "F3 0F 10 83 EC 3B 00 00 0F 2F C7 76 1B F3 0F 5C C6 F3 0F 11 83 EC 3B 00 00";
static constexpr const char* PAT_NTCHARGE_TICK  = "F3 0F 5D 86 74 07 00 00 F3 0F 11 86 74 07 00 00 80 BE 80 07 00 00 00";
static constexpr const char* PAT_NTCHARGE_REL   = "0F 2F 87 74 07 00 00 0F 87 94 00 00 00 48 8B 9F 68 18 00 00";

// Unique Technique limit 2 -> 4 (entry 521): 4 static patches
static constexpr const char* PAT_UQ_COUNT  = "03 D3 03 D7 03 D6 41 B8 02 00 00 00 41 3B D0 41 0F 47 D0 41 89 94 24 04 05 00 00";
static constexpr const char* PAT_UQ_MAX    = "8B 96 04 05 00 00 EB 02 33 D2 45 8B C6 48 8B CE 45 03 C0 48 8B 74 24 48";
static constexpr const char* PAT_UQ_SELECT = "83 FF 02 7C 58 48 8D 55 B7 48 8B CB E8 ?? ?? ?? ??";
static constexpr const char* PAT_UQ_CONFIRM= "83 FB 02 7C 3D 48 8D 15 ?? ?? ?? ??";

struct UqSite { uintptr_t addr; uint8_t orig[9]; uint8_t patched[9]; int len; };
static UqSite g_uqCount{}, g_uqMax{}, g_uqSelect{}, g_uqConfirm{};
static bool   g_uqResolved = false;
static bool   g_uqActive   = false;

static void ApplyUniqueLimitPatch(bool enable) {
    if (!g_uqResolved) { Beep(300, 300); WriteLog("[!] Unique-limit AOBs not resolved."); return; }
    const uint8_t* a = enable ? g_uqCount.patched   : g_uqCount.orig;
    const uint8_t* b = enable ? g_uqMax.patched     : g_uqMax.orig;
    const uint8_t* c = enable ? g_uqSelect.patched  : g_uqSelect.orig;
    const uint8_t* d = enable ? g_uqConfirm.patched : g_uqConfirm.orig;
    bool ok = true;
    ok &= WriteBytesRaw(g_uqCount.addr,   a, g_uqCount.len);
    ok &= WriteBytesRaw(g_uqMax.addr,     b, g_uqMax.len);
    ok &= WriteBytesRaw(g_uqSelect.addr,  c, g_uqSelect.len);
    ok &= WriteBytesRaw(g_uqConfirm.addr, d, g_uqConfirm.len);
    g_uqActive = enable && ok;
    WriteLog("[%c] Unique Technique limit %s", ok ? '+' : '!', enable ? "set to 4" : "reverted to 2");
    Beep(ok ? (enable ? 1400 : 700) : 300, 150);
}

// ============================================================
//  Install
// ============================================================
static bool InstallHooks(uintptr_t base, size_t size) {
    bool allOk = true;

    uintptr_t a;

    a = FindPattern(base, size, PAT_SHOT_TICK);
    if (a) { g_ret_shot_tick = WriteJmpHook(a, (void*)&ct2_shot_tick_hook, 8);
              WriteLog("[+] shot_tick @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] shot_tick AOB not found"); }

    a = FindPattern(base, size, PAT_SHOT_RELEASE);
    if (a) { g_ret_shot_release = WriteJmpHook(a, (void*)&ct2_shot_release_hook, 8);
              WriteLog("[+] shot_release @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] shot_release AOB not found"); }

    a = FindPattern(base, size, PAT_CHAIN_LVLSET);
    if (a) { g_ret_chain_level_set = WriteJmpHook(a, (void*)&ct2_chain_level_set_hook, 10);
              WriteLog("[+] chain_level_set @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] chain_level_set AOB not found"); }

    a = FindPattern(base, size, PAT_DUEL_DRIBBLE);
    if (a) { WriteJmpHook(a, (void*)&ct2_duel_dribble_hook, 7);
              WriteLog("[+] duel_dribble @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] duel_dribble AOB not found"); }

    a = FindPattern(base, size, PAT_SDRIBBLE_END);
    if (a) { g_ret_sdribble_end = WriteJmpHook(a, (void*)&ct2_sdribble_end_hook, 6);
              WriteLog("[+] sdribble_end @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] sdribble_end AOB not found"); }

    a = FindPattern(base, size, PAT_SDRIBBLE_DIR);
    if (a) { g_ret_sdribble_direct = WriteJmpHook(a, (void*)&ct2_sdribble_direct_hook, 6);
              WriteLog("[+] sdribble_direct @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] sdribble_direct AOB not found"); }

    a = FindPattern(base, size, PAT_SDRIBBLE_CD);
    if (a) { g_ret_sdribble_cooldown = WriteJmpHook(a, (void*)&ct2_sdribble_cooldown_hook, 8);
              WriteLog("[+] sdribble_cooldown @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] sdribble_cooldown AOB not found"); }

    a = FindPattern(base, size, PAT_NTCHARGE_TICK);
    if (a) { g_ret_ntcharge_tick = WriteJmpHook(a, (void*)&ct2_ntcharge_tick_hook, 8);
              WriteLog("[+] ntcharge_tick @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] ntcharge_tick AOB not found"); }

    a = FindPattern(base, size, PAT_NTCHARGE_REL);
    if (a) { g_ret_ntcharge_release = WriteJmpHook(a, (void*)&ct2_ntcharge_release_hook, 7);
              WriteLog("[+] ntcharge_release @ 0x%llX", (unsigned long long)a); }
    else { allOk = false; WriteLog("[!] ntcharge_release AOB not found"); }

    // --- Unique Technique limit: resolve sites now, apply later on F6 ---
    uintptr_t ca = FindPattern(base, size, PAT_UQ_COUNT);
    uintptr_t cb = FindPattern(base, size, PAT_UQ_MAX);
    uintptr_t cc = FindPattern(base, size, PAT_UQ_SELECT);
    uintptr_t cd = FindPattern(base, size, PAT_UQ_CONFIRM);
    if (ca && cb && cc && cd) {
        g_uqCount   = { ca + 8,  {2}, {4}, 1 };
        g_uqMax     = { cb + 0xA,
                         {0x45,0x8B,0xC6,0x48,0x8B,0xCE,0x45,0x03,0xC0},
                         {0x45,0x6B,0xC6,0x04,0x48,0x8B,0xCE,0x66,0x90}, 9 };
        g_uqSelect  = { cc + 2,  {2}, {4}, 1 };
        g_uqConfirm = { cd + 2,  {2}, {4}, 1 };
        g_uqResolved = true;
        WriteLog("[+] unique-limit sites resolved (count=0x%llX max=0x%llX select=0x%llX confirm=0x%llX)",
                 (unsigned long long)ca, (unsigned long long)cb, (unsigned long long)cc, (unsigned long long)cd);
    } else {
        allOk = false;
        WriteLog("[!] unique-limit AOBs not fully resolved");
    }

    return allOk;
}

// ============================================================
//  "Sta funzionando davvero" audio feedback
//  ------------------------------------------------------------
//  Gli hook in hooks.asm incrementano un contatore ogni volta che
//  applicano davvero l'effetto (non solo quando il tasto e' ON).
//  Qui, dal thread separato (MAI dal thread di gioco), confrontiamo
//  il contatore con l'ultimo valore visto: se e' cresciuto e sono
//  passati almeno DEBOUNCE_MS dall'ultimo bip per quella feature,
//  suoniamo un tono breve e distinto. Niente debounce = rumore
//  continuo indistinguibile durante azioni ripetute (es. dribbling).
// ============================================================
struct FireMonitor {
    const char* name;
    uint32_t*   counter;
    WORD        freqHz;
    uint32_t    lastSeen = 0;
    ULONGLONG   lastBeepTick = 0;
};

static constexpr DWORD DEBOUNCE_MS = 260;
static constexpr DWORD BEEP_MS     = 45;

static FireMonitor g_monitors[] = {
    { "Normal Tackle Instant Charge", &g_hits_ntcharge, 900  },
    { "Super Dribble Chain/Cooldown", &g_hits_sdribble, 1000 },
    { "PERFECT Dribble Grade",        &g_hits_dribble,  1100 },
    { "Instant Max-Charge Shot",      &g_hits_shot,     1250 },
    { "CHAIN MAX",                    &g_hits_chain,    1400 },
};

static void PollFireFeedback() {
    ULONGLONG now = GetTickCount64();
    for (auto& m : g_monitors) {
        uint32_t cur = *m.counter;
        if (cur != m.lastSeen) {
            m.lastSeen = cur;
            if (now - m.lastBeepTick >= DEBOUNCE_MS) {
                m.lastBeepTick = now;
                Beep(m.freqHz, BEEP_MS); // thread dedicato: non tocca il framerate di gioco
            }
        }
    }
}

// ============================================================
//  Hotkey loop
// ============================================================
static DWORD WINAPI MainThread(LPVOID) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) { WriteLog("[!] GetModuleHandleA NULL, abort."); return 1; }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;

    WriteLog("[i] %s v%s started. base=0x%llX size=0x%zX",
             TRAINER_NAME, TRAINER_VER, (unsigned long long)base, size);

    bool installed = InstallHooks(base, size);
    Beep(installed ? 1200 : 400, 200);
    WriteLog(installed ? "[i] All hooks installed." : "[!] Some hooks FAILED to install, see log above.");

    bool lastF1=false,lastF2=false,lastF3=false,lastF4=false,lastF5=false,lastF6=false,lastF9=false;

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { WriteLog("[i] ESC, stopping hotkey loop."); break; }

        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !lastF9) { WriteLog("[i] F9, unloading DLL."); Beep(400,150); FreeLibraryAndExitThread(g_hSelf, 0); }
        lastF9 = f9;

        bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1 && !lastF1) { g_flag_ntcharge ^= 1; WriteLog("[i] Normal Tackle Instant Charge: %s", g_flag_ntcharge?"ON":"OFF"); Beep(g_flag_ntcharge?900:450,120); }
        lastF1 = f1;

        bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2 && !lastF2) { g_flag_sdribble ^= 1; WriteLog("[i] Super Dribble Chain/Cooldown lock: %s", g_flag_sdribble?"ON":"OFF"); Beep(g_flag_sdribble?1000:500,120); }
        lastF2 = f2;

        bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !lastF3) { g_flag_dribble ^= 1; WriteLog("[i] Always PERFECT Dribble Grade: %s", g_flag_dribble?"ON":"OFF"); Beep(g_flag_dribble?1100:550,120); }
        lastF3 = f3;

        bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4 && !lastF4) { g_flag_shot ^= 1; WriteLog("[i] Instant Max-Charge Normal Shot: %s", g_flag_shot?"ON":"OFF"); Beep(g_flag_shot?1250:625,120); }
        lastF4 = f4;

        bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5 && !lastF5) { g_flag_chain ^= 1; WriteLog("[i] CHAIN MAX: %s", g_flag_chain?"ON":"OFF"); Beep(g_flag_chain?1400:700,120); }
        lastF5 = f5;

        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !lastF6) { ApplyUniqueLimitPatch(!g_uqActive); }
        lastF6 = f6;

        PollFireFeedback(); // bip breve e distinto ogni volta che un hook scatta davvero

        Sleep(8); // ~120 Hz
    }

    WriteLog("[i] Thread terminated (hooks remain live).");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
