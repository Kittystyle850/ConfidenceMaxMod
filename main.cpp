// Shadow of War (ShadowOfWar.exe, x64) - single-player mod DLL
// Port of SeiKur0's Cheat Engine table (v21) to a native DLL.
//
// Standalone: only the DLL is needed. No INI, no log, no dump files are created
// unless built with -DSOW_ENABLE_FILES=ON (debug aid). Settings are the constants
// in kConfig below (change them in source and rebuild).
//
// Features (default ON):
//   - Uruk spawn rarity      (legendary / epic)
//   - Get all Uruk intel
//   - Choose multiple skills
//   - Infinite mission time
//   - Forced drop            (Epic/Legendary Uruks always drop the NemesisGear
//                             entry named in kConfig.dropItem; off when empty)
//
// Every site is found by AOB scan and byte-verified before it is touched.
// Inject after the game has started (any injector).

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include "aob_scan.h"
#include "gamedb.h"
#include "stubs.h"

static HMODULE     g_self = nullptr;
static std::string g_dir;                 // folder of this DLL, trailing slash

struct Config {
    bool rarity = true, legendary = true;
    bool intel = true, skills = true, missionTime = true;
    bool dumpDrops = true;
    std::string dropItem;
};

static void Log(const char* fmt, ...) {
#ifdef SOW_ENABLE_FILES
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE* f = fopen((g_dir + "ShadowOfWarMod_log.txt").c_str(), "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    fclose(f);
#else
    (void)fmt;
#endif
}

// Built-in settings (no INI). Edit and rebuild to change.
static const Config kConfig = [] {
    Config c;
    c.rarity      = true;     // Uruk spawn rarity hook
    c.legendary   = true;     // true = legendary, false = epic
    c.intel       = true;     // all Uruk intel
    c.skills      = true;     // choose multiple skills
    c.missionTime = true;     // infinite mission time
    c.dropItem    = "";       // NemesisGear entry id to always drop ("" = off)
#ifdef SOW_ENABLE_FILES
    c.dumpDrops   = true;     // writes ShadowOfWarMod_drops.txt (debug build only)
#else
    c.dumpDrops   = false;
#endif
    return c;
}();

// ---------------------------------------------------------------------------
// jmp-hook through a near trampoline:  site: E9 rel32 (+nops) -> tramp:
//   FF 25 00000000 <abs64 stub>  -> stub ... jmp [ret slot] -> site+len
// ---------------------------------------------------------------------------
static uint8_t* g_tramp = nullptr;
static size_t   g_trampUsed = 0;

static bool InstallHook(HMODULE mod, const char* name, uint8_t* site, size_t len,
                        void* stub, uint64_t* retSlot) {
    if (len < 5 || len > 16) return false;
    if (!g_tramp) {
        g_tramp = AllocNearModule(mod, 0x1000);
        if (!g_tramp) { Log("[-] %s: no free memory near the module for the trampoline", name); return false; }
    }
    uint8_t* t = g_tramp + g_trampUsed;
    int64_t rel = (int64_t)(t - (site + 5));
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[-] %s: trampoline out of rel32 range", name); return false; }

    t[0] = 0xFF; t[1] = 0x25; *(uint32_t*)(t + 2) = 0; *(uint64_t*)(t + 6) = (uint64_t)stub;
    g_trampUsed += 16;

    *retSlot = (uint64_t)(site + len);                    // must be valid before the jmp goes live
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9; *(int32_t*)(patch + 1) = (int32_t)rel;
    if (!WriteCode(site, patch, len)) { Log("[-] %s: VirtualProtect/write failed", name); return false; }
    Log("[+] %s: hooked @ module+0x%llX", name, (unsigned long long)(site - (uint8_t*)mod));
    return true;
}

static bool PatchBytesAt(HMODULE mod, const char* name, uint8_t* site, const uint8_t* now,
                         const uint8_t* neu, size_t n) {
    if (!BytesEqual(site, now, n)) { Log("[-] %s: bytes at site differ from expected - skipped", name); return false; }
    if (!WriteCode(site, neu, n))  { Log("[-] %s: write failed", name); return false; }
    Log("[+] %s: patched @ module+0x%llX", name, (unsigned long long)(site - (uint8_t*)mod));
    return true;
}

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------
static bool ApplyRarity(HMODULE mod, bool legendary) {
    static const uint8_t prologue[] = { 0x57, 0x48, 0x83, 0xEC, 0x60 };            // push rdi; sub rsp,60
    static const uint8_t before[]   = { 0x48, 0x83, 0x64, 0x24, 0x28, 0x00 };      // and qword [rsp+28],0
    uint8_t* hit = FindPatternVerified(mod, "48 8D 4C 24 ?? 0F 57 C0 33 DB", true, [&](uint8_t* h) {
        return BytesEqual(h - 0x1E, prologue, sizeof(prologue)) && BytesEqual(h - 6, before, sizeof(before));
    });
    if (!hit) return false;
    g_rarityLegendary = legendary ? 1 : 0;
    return InstallHook(mod, "uruk_spawn_rarity", hit - 0x1E, 5, (void*)&SowRarityStub, &ret_rarity);
}

static bool ApplyIntel(HMODULE mod) {
    static const uint8_t orig[] = { 0xBA, 0x03, 0x00, 0x00, 0x00 };                // mov edx,3
    uint8_t* hit = FindPatternVerified(mod, "B0 01 EB ?? 48 8D 8B ?? ?? ?? ?? BA 03", true, [&](uint8_t* h) {
        return BytesEqual(h + 0xB, orig, sizeof(orig));
    });
    if (!hit) return false;
    return InstallHook(mod, "all_uruk_intel", hit + 0xB, 5, (void*)&SowIntelStub, &ret_intel);
}

static bool ApplySkills(HMODULE mod) {
    uint8_t* hit = FindPatternInModule(mod,
        "74 ?? 48 8B ?? 40 04 00 00 48 85 ?? 74 ?? 4C 39 ?? 40 74 ?? ?? ?? ?? ?? ?? ?? ?? ?? 08 00 00 00");
    if (!hit) return false;
    const uint8_t now[] = { 0x74 }, neu[] = { 0xEB };                              // je -> jmp
    return PatchBytesAt(mod, "choose_multiple_skills", hit, now, neu, 1);
}

static bool ApplyMissionTime(HMODULE mod) {
    uint8_t* hit = FindPatternInModule(mod, "48 8D 59 10 48 8B F9 48 8B CB E8 ?? ?? ?? ?? 84 C0 0F 85");
    if (!hit) return false;
    const uint8_t now[] = { 0x84, 0xC0 }, neu[] = { 0xEB, 0x06 };                  // test al,al -> jmp +6
    return PatchBytesAt(mod, "infinite_mission_time", hit + 0xF, now, neu, 2);
}

// returns true when finished (success OR permanent failure), false = retry later
static bool ApplyDrops(HMODULE mod, const Config& cfg) {
#ifdef SOW_ENABLE_FILES
    static bool dumped = false;
#endif
    uintptr_t list = 0;
    uintptr_t db = FindGameDb(mod, "Faction/Loot/NemesisGear", list);
    if (!db) return false;                                  // gamedb not loaded yet

#ifdef SOW_ENABLE_FILES
    if (cfg.dumpDrops && !dumped) {
        std::string txt = DbDumpList(list);
        FILE* f = fopen((g_dir + "ShadowOfWarMod_drops.txt").c_str(), "w");
        if (f) {
            fprintf(f, "# Faction/Loot/NemesisGear  (index<TAB>id) - put an id in [Drops] ItemId\n%s", txt.c_str());
            fclose(f);
        }
        Log("[i] drop list dumped (%zu bytes) -> ShadowOfWarMod_drops.txt", txt.size());
        dumped = true;
    }
#endif
    if (cfg.dropItem.empty()) { Log("[i] dropItem empty - forced drop not installed"); return true; }

    uintptr_t entry = DbEntryBase(list, cfg.dropItem);
    if (!entry) { Log("[-] drop item '%s' not found in NemesisGear - check the dump file", cfg.dropItem.c_str()); return true; }

    static const uint8_t orig[] = { 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0 };          // mov rbx,rax; test rax,rax
    uint8_t* hit = FindPatternVerified(mod,
        "48 8D 4C 24 ?? 49 8B D4 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74", true, [&](uint8_t* h) {
            return BytesEqual(h + 0xD, orig, sizeof(orig));
        });
    if (!hit) return false;
    g_lootNemGear = (uint64_t)list;
    g_dropItem    = (uint64_t)entry;
    if (!InstallHook(mod, "forced_drop", hit + 0xD, 6, (void*)&SowDropStub, &ret_drop)) {
        g_dropItem = 0;
        return true;
    }
    Log("[+] forced drop item '%s' @ 0x%llX", cfg.dropItem.c_str(), (unsigned long long)entry);
    return true;
}

static DWORD WINAPI MainThread(LPVOID) {
    const Config& cfg = kConfig;
    Log("[i] ShadowOfWarMod started (rarity=%d/%s intel=%d skills=%d time=%d drop='%s')",
        cfg.rarity, cfg.legendary ? "legendary" : "epic", cfg.intel, cfg.skills, cfg.missionTime,
        cfg.dropItem.c_str());

    HMODULE mod = nullptr;
    for (int i = 0; i < 240 && !mod; ++i) {
        mod = GetModuleHandleA("ShadowOfWar.exe");
        if (!mod) Sleep(500);
    }
    if (!mod) { Log("[-] ShadowOfWar.exe not found in this process - wrong target?"); return 0; }
    Log("[i] module base=0x%p", (void*)mod);

    bool dRarity = !cfg.rarity, dIntel = !cfg.intel, dSkills = !cfg.skills, dTime = !cfg.missionTime;
    bool dDrops = !cfg.dumpDrops && cfg.dropItem.empty();

    const int maxTries = 300;                                // ~5 min, covers slow game start
    for (int t = 0; t < maxTries; ++t) {
        if (!dRarity) dRarity = ApplyRarity(mod, cfg.legendary);
        if (!dIntel)  dIntel  = ApplyIntel(mod);
        if (!dSkills) dSkills = ApplySkills(mod);
        if (!dTime)   dTime   = ApplyMissionTime(mod);
        if (!dDrops)  dDrops  = ApplyDrops(mod, cfg);
        if (dRarity && dIntel && dSkills && dTime && dDrops) break;
        Sleep(1000);
    }
    if (!dRarity) Log("[-] uruk_spawn_rarity: AOB not found (game version mismatch?)");
    if (!dIntel)  Log("[-] all_uruk_intel: AOB not found (game version mismatch?)");
    if (!dSkills) Log("[-] choose_multiple_skills: AOB not found (game version mismatch?)");
    if (!dTime)   Log("[-] infinite_mission_time: AOB not found (game version mismatch?)");
    if (!dDrops)  Log("[-] drops: gamedb or hook AOB not found");
    Log("[i] done.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_self = hModule;
        char path[MAX_PATH];
        GetModuleFileNameA(hModule, path, MAX_PATH);
        g_dir = path;
        size_t pos = g_dir.find_last_of("\\/");
        g_dir = (pos == std::string::npos) ? "" : g_dir.substr(0, pos + 1);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
