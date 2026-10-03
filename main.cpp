// ============================================================
//  CT2CheatDLL v2.0  -  Captain Tsubasa 2: World Fighters (x64)
//  ------------------------------------------------------------
//  Trimmed build: only two effects remain, both ON by default,
//  no hotkeys, no beeps.
//    - CHAIN MAX (shared, both teams)
//    - Unique Technique limit 2 -> 4 (normal slots)
//  Works unmodified against both uploaded targets (Crack + Steam):
//  the gameplay code section is byte-identical between the two.
//
//  Log: CT2CheatDLL_log.txt (next to the game EXE)
// ============================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// ---- forward decl needed by custom_player_editor.h before WriteLog's
//      definition further down (non-static: the header calls it too) ----
void WriteLog(const char* fmt, ...);

// ---- symbols defined in hooks.asm ----
extern "C" {
    extern uint8_t  g_flag_chain;          // defaults to 1 (ON) in hooks.asm
    extern uint64_t g_ret_chain_level_set;
    void ct2_chain_level_set_hook();
}

#include "custom_player_editor.h"
#include "player_scanner.h"

static constexpr const char* TRAINER_NAME = "CT2CheatDLL";
static constexpr const char* TRAINER_VER  = "2.0";

static HMODULE g_hSelf = nullptr;

// ============================================================
//  Log
// ============================================================
void WriteLog(const char* fmt, ...) {
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
        if (s[0] == '?') {
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
//  Near-memory trampoline allocator (see v1.1 changelog in README
//  for why this exists: the DLL can land far outside +-2GB of the
//  game module, which is farther than a plain E9 jmp can reach).
// ============================================================
struct NearStubAllocator {
    uint8_t* page = nullptr;
    size_t   used = 0;
    static constexpr size_t kPageSize = 0x1000;

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
                    WriteLog("[i] near-stub page allocated @ 0x%llX (target 0x%llX, delta %+lld bytes)",
                             (unsigned long long)p, (unsigned long long)nearTarget, (long long)diff);
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

// Writes E9 rel32 -> target at addr (+NOP padding to patchLen), transparently
// routed via a near trampoline if target is out of +-2GB range. Returns
// addr+patchLen on success (the "return address" to resume original
// execution from), or 0 on failure.
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
    for (int i = 5; i < patchLen; ++i) p[i] = 0x90;
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
//  AOB patterns (identical in both the Crack and Steam tables)
// ============================================================
static constexpr const char* PAT_CHAIN_LVLSET = "48 89 5C 24 18 48 89 6C 24 20 56 57 41 57 48 83 EC 50 48 63 41 24";

static constexpr const char* PAT_UQ_COUNT  = "03 D3 03 D7 03 D6 41 B8 02 00 00 00 41 3B D0 41 0F 47 D0 41 89 94 24 04 05 00 00";
static constexpr const char* PAT_UQ_MAX    = "8B 96 04 05 00 00 EB 02 33 D2 45 8B C6 48 8B CE 45 03 C0 48 8B 74 24 48";
static constexpr const char* PAT_UQ_SELECT = "83 FF 02 7C 58 48 8D 55 B7 48 8B CB E8 ?? ?? ?? ??";
static constexpr const char* PAT_UQ_CONFIRM= "83 FB 02 7C 3D 48 8D 15 ?? ?? ?? ??";

struct UqSite { uintptr_t addr; uint8_t orig[9]; uint8_t patched[9]; int len; };
static UqSite g_uqCount{}, g_uqMax{}, g_uqSelect{}, g_uqConfirm{};

static bool ApplyUniqueLimitPatch() {
    bool ok = true;
    ok &= WriteBytesRaw(g_uqCount.addr,   g_uqCount.patched,   g_uqCount.len);
    ok &= WriteBytesRaw(g_uqMax.addr,     g_uqMax.patched,     g_uqMax.len);
    ok &= WriteBytesRaw(g_uqSelect.addr,  g_uqSelect.patched,  g_uqSelect.len);
    ok &= WriteBytesRaw(g_uqConfirm.addr, g_uqConfirm.patched, g_uqConfirm.len);
    WriteLog("[%c] Unique Technique limit set to 4", ok ? '+' : '!');
    return ok;
}

// ============================================================
//  Install
// ============================================================
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

static bool InstallAll(uintptr_t base, size_t size) {
    bool allOk = true;

    allOk &= InstallOne("chain_level_set", base, size, PAT_CHAIN_LVLSET,
                         (void*)&ct2_chain_level_set_hook, 10, &g_ret_chain_level_set);

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
        WriteLog("[+] %-18s @ 0x%llX / 0x%llX / 0x%llX / 0x%llX",
                 "unique_limit", (unsigned long long)ca, (unsigned long long)cb,
                 (unsigned long long)cc, (unsigned long long)cd);
        allOk &= ApplyUniqueLimitPatch();
    } else {
        allOk = false;
        WriteLog("[!] %-18s not fully resolved (count=%d max=%d select=%d confirm=%d)",
                 "unique_limit", ca != 0, cb != 0, cc != 0, cd != 0);
    }

    return allOk;
}

// ============================================================
//  Thread: installs once, then only watches for F9/ESC (no cheat
//  hotkeys left - both effects are default-on, no beeps).
// ============================================================
static DWORD WINAPI MainThread(LPVOID) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) { WriteLog("[!] GetModuleHandleA NULL, abort."); return 1; }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;

    WriteLog("[i] %s v%s started. base=0x%llX size=0x%zX",
             TRAINER_NAME, TRAINER_VER, (unsigned long long)base, size);

    bool installed = InstallAll(base, size);
    WriteLog(installed
        ? "[i] CHAIN MAX + Unique Technique Limit=4 both active. No hotkeys, no sound."
        : "[!] Something FAILED - look for '[!]' lines above.");

    // Custom Player live editor: embedded move-name databases, reference
    // lists, default config file, and a watcher that re-applies the
    // config whenever its content actually changes.
    CustomPlayerEditor::g_db.BuildAll();
    CustomPlayerEditor::WriteAllRefFiles();
    CustomPlayerEditor::WriteDefaultConfigIfMissing();
    WriteLog("[i] CustomPlayerEditor ready. Edit CT2_CustomPlayerMoves.txt (names from "
             "CT2_MoveList_*.txt) - picked up automatically.");

    WriteLog("[i] PlayerScanner ready. Press F8 on a roster/squad screen to hunt for the full "
             "player array - writes CT2_PlayerScan_Report.txt.");

    FILETIME lastWrite{};
    bool lastF9 = false;
    bool lastF8 = false;
    int pollTick = 0;
    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { WriteLog("[i] ESC, stopping watch loop."); break; }
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !lastF9) { WriteLog("[i] F9, unloading DLL."); FreeLibraryAndExitThread(g_hSelf, 0); }
        lastF9 = f9;

        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !lastF8) PlayerScanner::RunScanAndReport();
        lastF8 = f8;

        // Check the config file's mtime roughly twice a second; only
        // re-apply when it actually changed, so the log doesn't spam.
        if (++pollTick >= 15) {
            pollTick = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExA(CustomPlayerEditor::CONFIG_PATH, GetFileExInfoStandard, &fad)) {
                if (CompareFileTime(&fad.ftLastWriteTime, &lastWrite) != 0) {
                    lastWrite = fad.ftLastWriteTime;
                    CustomPlayerEditor::ApplyConfig();
                }
            }
        }

        Sleep(30);
    }

    WriteLog("[i] Watch loop terminated (hooks and patches remain live).");
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
