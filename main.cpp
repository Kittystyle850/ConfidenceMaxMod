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
// ------------------------------------------------------------
//  Near-memory trampoline allocator.
//  ------------------------------------------------------------
//  A DLL loaded via LoadLibrary can land anywhere in the process'
//  address space; on this game it was observed ~0x7C80000000 bytes
//  away from the module base, far outside the +-2GB an E9 rel32 jmp
//  can reach. Rather than relocating our codecaves (which would
//  break every RIP-relative reference to the flag/return-pointer
//  globals), we reserve one small RWX page close to the game module
//  and bump-allocate 14-byte absolute-jump stubs from it on demand:
//      FF 25 00 00 00 00      ; jmp qword ptr [rip+0]
//      <8 bytes: absolute target address>
//  The patched game code jmps (in range) to the stub, which then
//  jmps (unrestricted, absolute) to the real codecave.
// ------------------------------------------------------------
struct NearStubAllocator {
    uint8_t* page = nullptr;
    size_t   used = 0;
    static constexpr size_t kPageSize = 0x1000; // room for ~290 stubs, we need <20

    bool EnsurePage(uintptr_t nearTarget) {
        if (page) return true;
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity;
        for (uintptr_t delta = 0; delta < 0x70000000ULL; delta += gran) {
            for (int sign = -1; sign <= 1; sign += 2) {
                uintptr_t cand = nearTarget + static_cast<uintptr_t>(static_cast<int64_t>(sign) * static_cast<int64_t>(delta));
                cand &= ~static_cast<uintptr_t>(gran - 1);
                void* p = VirtualAlloc(reinterpret_cast<LPVOID>(cand), kPageSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (!p) continue;
                int64_t diff = static_cast<int64_t>(reinterpret_cast<uintptr_t>(p)) - static_cast<int64_t>(nearTarget);
                if (diff > -0x70000000LL && diff < 0x70000000LL) {
                    page = reinterpret_cast<uint8_t*>(p);
                    WriteLog("[i] near-stub page allocated @ 0x%llX (target 0x%llX, delta 0x%llX)",
                             (unsigned long long)p, (unsigned long long)nearTarget, (unsigned long long)diff);
                    return true;
                }
                VirtualFree(p, 0, MEM_RELEASE);
            }
        }
        return false;
    }

    void* MakeTrampoline(uintptr_t nearTarget, void* realTarget) {
        if (!EnsurePage(nearTarget)) return nullptr;
        if (used + 14 > kPageSize) { WriteLog("[!] near-stub page exhausted"); return nullptr; }
        uint8_t* stub = page + used;
        used += 14;
        stub[0] = 0xFF; stub[1] = 0x25;
        *reinterpret_cast<uint32_t*>(stub + 2) = 0;
        *reinterpret_cast<uint64_t*>(stub + 6) = reinterpret_cast<uint64_t>(realTarget);
        FlushInstructionCache(GetCurrentProcess(), stub, 14);
        return stub;
    }
};
static NearStubAllocator g_stubs;

// Writes E9 rel32 -> target at addr (+NOP padding to patchLen). If target is
// out of +-2GB range, transparently routes through a near trampoline first.
// Returns addr+patchLen on success (the mid-function "return address" to
// resume original execution from), or 0 on failure.
static uintptr_t WriteJmpHook(uintptr_t addr, void* target, int patchLen) {
    if (patchLen < 5) { WriteLog("[!] patchLen<5 @ 0x%llX", (unsigned long long)addr); return 0; }

    int64_t rel = reinterpret_cast<int64_t>(target) - (static_cast<int64_t>(addr) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        void* stub = g_stubs.MakeTrampoline(addr, target);
        if (!stub) {
            WriteLog("[!] out of range @ 0x%llX (target 0x%llX) and stub allocation FAILED",
                     (unsigned long long)addr, (unsigned long long)target);
            return 0;
        }
        WriteLog("[i] 0x%llX too far from 0x%llX, routed via near stub @ 0x%llX",
                 (unsigned long long)addr, (unsigned long long)target, (unsigned long long)stub);
        target = stub;
        rel = reinterpret_cast<int64_t>(target) - (static_cast<int64_t>(addr) + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            WriteLog("[!] stub still out of range, giving up @ 0x%llX", (unsigned long long)addr);
            return 0;
        }
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), patchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        WriteLog("[!] VirtualProtect failed @ 0x%llX (err=%lu)", (unsigned long long)addr, GetLastError());
        return 0;
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
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

// Finds `pattern`, writes the jmp hook (transparently via a near
// stub if needed) and logs success/failure honestly — this used to
// be duplicated 9x with the success log printed unconditionally
// even when WriteJmpHook had just failed and returned 0.
static bool InstallOne(const char* name, uintptr_t base, size_t size,
                        const char* pattern, void* hookFn, int patchLen,
                        uint64_t* retSlot) {
    uintptr_t a = FindPattern(base, size, pattern);
    if (!a) { WriteLog("[!] %-18s AOB not found", name); return false; }

    uintptr_t ret = WriteJmpHook(a, hookFn, patchLen);
    if (!ret) { WriteLog("[!] %-18s FOUND @ 0x%llX but hook install FAILED", name, (unsigned long long)a); return false; }

    if (retSlot) *retSlot = ret;
    WriteLog("[+] %-18s @ 0x%llX", name, (unsigned long long)a);
    return true;
}

static bool InstallHooks(uintptr_t base, size_t size) {
    bool allOk = true;

    allOk &= InstallOne("shot_tick",         base, size, PAT_SHOT_TICK,    (void*)&ct2_shot_tick_hook,        8,  &g_ret_shot_tick);
    allOk &= InstallOne("shot_release",      base, size, PAT_SHOT_RELEASE, (void*)&ct2_shot_release_hook,     8,  &g_ret_shot_release);
    allOk &= InstallOne("chain_level_set",   base, size, PAT_CHAIN_LVLSET, (void*)&ct2_chain_level_set_hook,  10, &g_ret_chain_level_set);
    allOk &= InstallOne("duel_dribble",      base, size, PAT_DUEL_DRIBBLE, (void*)&ct2_duel_dribble_hook,     7,  nullptr);
    allOk &= InstallOne("sdribble_end",      base, size, PAT_SDRIBBLE_END, (void*)&ct2_sdribble_end_hook,     6,  &g_ret_sdribble_end);
    allOk &= InstallOne("sdribble_direct",   base, size, PAT_SDRIBBLE_DIR, (void*)&ct2_sdribble_direct_hook,  6,  &g_ret_sdribble_direct);
    allOk &= InstallOne("sdribble_cooldown", base, size, PAT_SDRIBBLE_CD,  (void*)&ct2_sdribble_cooldown_hook,8,  &g_ret_sdribble_cooldown);
    allOk &= InstallOne("ntcharge_tick",     base, size, PAT_NTCHARGE_TICK,(void*)&ct2_ntcharge_tick_hook,    8,  &g_ret_ntcharge_tick);
    allOk &= InstallOne("ntcharge_release",  base, size, PAT_NTCHARGE_REL, (void*)&ct2_ntcharge_release_hook, 7,  &g_ret_ntcharge_release);

    // --- Unique Technique limit: resolve sites now, apply later on F6 ---
    // (plain byte patch, no jmp involved, so no +-2GB concern here)
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
        WriteLog("[+] %-18s @ 0x%llX / 0x%llX / 0x%llX / 0x%llX",
                 "unique_limit", (unsigned long long)ca, (unsigned long long)cb,
                 (unsigned long long)cc, (unsigned long long)cd);
    } else {
        allOk = false;
        WriteLog("[!] %-18s not fully resolved (count=%d max=%d select=%d confirm=%d)",
                 "unique_limit", ca != 0, cb != 0, cc != 0, cd != 0);
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
    WriteLog(installed
        ? "[i] All hooks installed OK. Check lines above for any '[i] ... routed via near stub' - normal, not an error."
        : "[!] One or more hooks FAILED - look for '[!]' lines above, that's exactly which cheat(s) won't work.");

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
