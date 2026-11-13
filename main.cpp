// Onimusha: Way of the Sword - DUMP DLL (Strada C).
// Read-only: hooks only observe, never write to game state.
// Logs every state change: ACTION_STATE_BIT names, action vtable, cancel phases.
// Goal: identify the real action object for attack_light / attack_hard /
// dodge / guard / parry, and see whether the cancel phases are populated.
//
// Target: Steam 24769601 / 01.000.001, TimeDateStamp 0x6A827CEA,
// SizeOfImage 0x249F0000, image base 0x140000000.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include "MinHook.h"

// ─────────────────────────────────────────────────────────────
static constexpr DWORD     kTimestamp   = 0x6A827CEA;
static constexpr DWORD     kSizeOfImage = 0x249F0000;
static constexpr uintptr_t kBase        = 0x140000000;

// ─────────────────────────────────────────────────────────────
// RVAs
// ─────────────────────────────────────────────────────────────
static constexpr uintptr_t RVA_PCC_UPDATE           = 0x2015F0;
static constexpr uintptr_t RVA_PAS_UPDATE           = 0x455340;
static constexpr uintptr_t RVA_CB_GETACTIONSTATE    = 0x49A1D10;
static constexpr uintptr_t RVA_CB_GETBASEACTION     = 0x6669E30;
static constexpr uintptr_t RVA_PCE_GETCANCELSEQ     = 0x6795B20;

// ─────────────────────────────────────────────────────────────
// Field offsets
// ─────────────────────────────────────────────────────────────
static constexpr uintptr_t OFF_CCCMD_PHASE  = 0x10;
static constexpr uintptr_t OFF_PC_ENTITY    = 0x1F0;   // PlayerCharacter._PlayerCharacterEntity
static constexpr uintptr_t OFF_PCE_SEQ      = 0x5B8;
static constexpr uintptr_t OFF_ARR_COUNT    = 0x1C;
static constexpr uintptr_t OFF_ARR_DATA     = 0x20;

// ─────────────────────────────────────────────────────────────
// ACTION_STATE_BIT names (from REFramework dump)
// ─────────────────────────────────────────────────────────────
struct StateBit { uint64_t mask; const char* name; };
static const StateBit kStateBits[] = {
    { 0x1ull,         "STAND" },
    { 0x2ull,         "CROUCH" },
    { 0x4ull,         "AIR" },
    { 0x8ull,         "DOWN" },
    { 0x10ull,        "WALL" },
    { 0x20ull,        "WATER" },
    { 0x40ull,        "DIVING" },
    { 0x10000ull,     "IDLE" },
    { 0x20000ull,     "MOVING" },
    { 0x40000ull,     "ATTACK" },
    { 0x80000ull,     "GRAPPLE" },
    { 0x100000ull,    "DAMAGE" },
    { 0x200000ull,    "SMASH" },
    { 0x400000ull,    "BLOCKED" },
    { 0x800000ull,    "PARRIED" },
    { 0x1000000ull,   "GUARD_WAIT" },
    { 0x2000000ull,   "GUARD_HIT" },
    { 0x4000000ull,   "GUARD_BREAK" },
    { 0x8000000ull,   "BLOCK" },
    { 0x10000000ull,  "PARRY" },
    { 0x20000000ull,  "EVADE" },
    { 0x40000000ull,  "SHELL_ATTACK" },
    { 0x80000000ull,  "ATEMI" },
    { 0x100000000ull, "FEINT" },
    { 0x200000000ull, "EM_GRAB" },
    { 0x400000000ull, "EM_UNAVOIDABLE" },
    { 0x800000000ull, "EM_ATTACK_AFTER_GRAPPLE" },
};
static constexpr size_t kStateBitsCount = sizeof(kStateBits) / sizeof(kStateBits[0]);

static std::string StateStr(int64_t s) {
    if (s == 0) return "NONE";
    std::string r;
    for (size_t i = 0; i < kStateBitsCount; i++) {
        if (s & (int64_t)kStateBits[i].mask) {
            if (!r.empty()) r += "|";
            r += kStateBits[i].name;
        }
    }
    return r.empty() ? "UNKNOWN" : r;
}

// ─────────────────────────────────────────────────────────────
// Cancel key names (from dump1)
// ─────────────────────────────────────────────────────────────
struct KeyName { uintptr_t offset; const char* name; };
static const KeyName kKeyNames[] = {
    { 0x50, "_OniChangeCancel" },          { 0x58, "_OniEyeSonerCancel" },
    { 0x60, "_ParryCancel" },              { 0x68, "_BlockCancel" },
    { 0x70, "_JustDodgeCancel" },          { 0x78, "_GrabCounterCancel" },
    { 0x80, "_IssenCounterCancel" },       { 0x88, "_IssenBreakCancel" },
    { 0x90, "_IssenBlockCancel" },         { 0x98, "_IssenChainCancel" },
    { 0xA0, "_GrappleBlowCancel" },        { 0xA8, "_SkillCancel" },
    { 0xB0, "_SkillKeepCancel" },          { 0xB8, "_UltimateAttackCancel" },
    { 0xC0, "_AbsorbCancel" },             { 0xC8, "_BowCancel" },
    { 0xD0, "_GuardCancel" },              { 0xD8, "_AttackBlowCancel" },
    { 0xE0, "_DodgeCancel" },              { 0xE8, "_AttackJustDodgeCancel" },
    { 0xF0, "_AttackSneakCancel" },        { 0xF8, "_AttackLightChargeCancel" },
    { 0x100, "_AttackLightComboCancel" },  { 0x108, "_AttackLightCancel" },
    { 0x110, "_AttackHardChargeCancel" },  { 0x118, "_AttackHardComboCancel" },
    { 0x120, "_AttackHardCancel" },        { 0x128, "_MedicineCancel" },
    { 0x130, "_ItemCancel" },              { 0x138, "_InteractCancel" },
    { 0x140, "_InteractKeepCancel" },      { 0x148, "_DashCancel" },
    { 0x150, "_DashKeepCancel" },          { 0x158, "_RunCancel" },
    { 0x160, "_RunTurnCancel" },           { 0x168, "_WalkCancel" },
    { 0x170, "_WalkKeepCancel" },          { 0x178, "_WalkStickCancel" },
    { 0x180, "_QuickStopCancel" },         { 0x188, "_MenuItemCancel" },
    { 0x190, "_MenuSkillCancel" },         { 0x198, "_LockOnCancel" },
};
static constexpr size_t kKeysCount = sizeof(kKeyNames) / sizeof(kKeyNames[0]);

// ─────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────
struct Config {
    bool enabled = true;
    bool log_on = true;
    bool hotkey_f1 = true;
    int  log_every_frames = 60;    // 1 summary per second at 60fps
} g_cfg;

// ─────────────────────────────────────────────────────────────
// Runtime state
// ─────────────────────────────────────────────────────────────
struct RuntimeState {
    void*    entity = nullptr;
    void*    player = nullptr;
    void*    seq = nullptr;
    uint32_t seq_count = 0;
    void*    last_action = nullptr;
    uintptr_t last_vtable = 0;
    int64_t  last_state = 0;
    uint64_t frame = 0;
    uint64_t last_summary = 0;
    bool     f1_prev = false;
    std::unordered_map<uintptr_t, bool> layer_seen;
} g_rt;

// ─────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────
static std::string g_log_path;
static void Log(const char* fmt, ...) {
    if (!g_cfg.log_on) return;
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE* f = fopen(g_log_path.c_str(), "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

// ─────────────────────────────────────────────────────────────
// Safe memory access
// ─────────────────────────────────────────────────────────────
template<typename T>
static bool SafeRead(void* base, uintptr_t off, T& out) {
    __try { out = *(T*)((uintptr_t)base + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static inline void* RdPtr(void* b, uintptr_t o) {
    void* v = nullptr; return SafeRead(b, o, v) ? v : nullptr;
}
static inline int32_t RdI32(void* b, uintptr_t o, int32_t d = 0) {
    int32_t v = d; return SafeRead(b, o, v) ? v : d;
}
static inline uintptr_t RdUP(void* b, uintptr_t o, uintptr_t d = 0) {
    uintptr_t v = d; return SafeRead(b, o, v) ? v : d;
}

// ─────────────────────────────────────────────────────────────
// INI loader (idempotent, F1 hotkey handled elsewhere)
// ─────────────────────────────────────────────────────────────
static std::string g_ini_path;
static void LoadConfig(bool initial) {
    if (g_ini_path.empty()) {
        char p[MAX_PATH]; HMODULE m = GetModuleHandleA("OnimushaMod.dll");
        if (!m) m = GetModuleHandleA(nullptr);
        GetModuleFileNameA(m, p, MAX_PATH);
        std::string s = p; auto d = s.find_last_of('.');
        g_ini_path = (d == std::string::npos) ? s + ".ini" : s.substr(0, d) + ".ini";
    }
    const char* ini = g_ini_path.c_str();
    g_cfg.enabled          = GetPrivateProfileIntA("Dump", "Enabled", 1, ini) != 0;
    g_cfg.log_on           = GetPrivateProfileIntA("Debug", "Log", 1, ini) != 0;
    g_cfg.hotkey_f1        = GetPrivateProfileIntA("Debug", "HotkeyF1", 1, ini) != 0;
    g_cfg.log_every_frames = GetPrivateProfileIntA("Dump", "LogEveryFrames", 60, ini);
    if (initial) Log("[cfg] dump enabled=%d log_every=%d", g_cfg.enabled, g_cfg.log_every_frames);
    else Log("[cfg] reload complete");
}

// ─────────────────────────────────────────────────────────────
// Game method wrappers (read-only, SEH-guarded)
// ─────────────────────────────────────────────────────────────
typedef int64_t (*GetStateFn)(void*);
typedef void*   (*GetBaseActionFn)(void*);
typedef void*   (*GetSeqFn)(void*);
typedef void    (*CtrlUpFn)(void*, void*);
typedef void    (*SelUpFn)(void*);

static GetStateFn     o_GetState = nullptr;
static GetBaseActionFn o_GetBaseAction = nullptr;
static GetSeqFn       o_GetSeq   = nullptr;
static CtrlUpFn       o_CtrlUp   = nullptr;
static SelUpFn        o_SelUp    = nullptr;

// ─────────────────────────────────────────────────────────────
// Layer / key dump helpers
// ─────────────────────────────────────────────────────────────
static void DumpLayerPhases(void* layer, uint32_t idx) {
    if (!layer) return;
    std::string line;
    int nonzero = 0;
    for (const auto& kn : kKeyNames) {
        void* key_obj = RdPtr(layer, kn.offset);
        if (!key_obj) continue;
        int32_t phase = RdI32(key_obj, OFF_CCCMD_PHASE, -1);
        if (phase < 0) continue;
        if (phase != 0) {
            if (!line.empty()) line += " ";
            char buf[64];
            snprintf(buf, sizeof(buf), "%s=%d", kn.name + 1, phase);   // strip leading _
            line += buf;
            nonzero++;
        }
    }
    if (nonzero > 0) {
        Log("[phases] L%u %p nonNONE(%d): %s", idx, layer, nonzero, line.c_str());
    } else {
        Log("[phases] L%u %p all NONE", idx, layer);
    }
}

static void RefreshSeq() {
    if (!g_rt.entity) { g_rt.seq = nullptr; g_rt.seq_count = 0; return; }
    void* seq = RdPtr(g_rt.entity, OFF_PCE_SEQ);
    uintptr_t sa = (uintptr_t)seq;
    bool ok = seq && sa >= 0x10000 && sa <= 0x7FFFFFFFFFFF && (sa & 7) == 0;
    if (!ok) { g_rt.seq = nullptr; g_rt.seq_count = 0; return; }
    if (seq == g_rt.seq) return;
    g_rt.seq = seq;
    g_rt.seq_count = 0;
    g_rt.layer_seen.clear();
    int32_t cnt = RdI32(seq, OFF_ARR_COUNT);
    if (cnt > 0 && cnt < 128) g_rt.seq_count = (uint32_t)cnt;
    Log("[seq] changed -> %p layers=%u", seq, g_rt.seq_count);
}

// ─────────────────────────────────────────────────────────────
// Hooks (read-only, never modify game state)
// ─────────────────────────────────────────────────────────────

// getActionState(this=player) -> int64_t
static int64_t hk_GetState(void* player) {
    int64_t s = 0;
    if (o_GetState) {
        __try { s = o_GetState(player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { s = 0; }
    }
    if (player && !g_rt.player) {
        g_rt.player = player;
        g_rt.entity = RdPtr(player, OFF_PC_ENTITY);
        Log("[capture] player=%p entity=%p", player, g_rt.entity);
    }
    return s;
}

// get_BaseCurrentAction(this=player) -> action*
static void* hk_GetBaseAction(void* self) {
    void* action = nullptr;
    if (o_GetBaseAction) {
        __try { action = o_GetBaseAction(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (action && action != g_rt.last_action) {
        uintptr_t vtbl = RdUP(action, 0);
        g_rt.last_action = action;
        g_rt.last_vtable = vtbl;
        Log("[action] ptr=%p vtable=0x%llX", action, (unsigned long long)vtbl);
    }
    return action;
}

// getCancelMotionSeq(this=entity) -> seq*
static void* hk_GetSeq(void* entity) {
    void* seq = nullptr;
    if (o_GetSeq) {
        __try { seq = o_GetSeq(entity); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (entity && !g_rt.entity) {
        g_rt.entity = entity;
        Log("[capture] entity=%p (from getCancelMotionSeq)", entity);
    }
    return seq;
}

// ctrlUpdate(this, mask) - dump summary
static void hk_CtrlUp(void* self, void* mask) {
    g_rt.frame++;
    if (g_cfg.hotkey_f1) {
        bool d = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (d && !g_rt.f1_prev) {
            g_cfg.enabled = !g_cfg.enabled;
            Log("[hotkey] F1 -> dump=%d", g_cfg.enabled);
        }
        g_rt.f1_prev = d;
    }
    if (g_cfg.enabled) {
        // call getActionState directly if we have player
        int64_t s = 0;
        if (g_rt.player && o_GetState) {
            __try { s = o_GetState(g_rt.player); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // log on state change
        if (s != g_rt.last_state) {
            Log("[state] 0x%llX %s", (unsigned long long)s, StateStr(s).c_str());
            g_rt.last_state = s;
            // on every state change, dump phase keys of all layers
            RefreshSeq();
            if (g_rt.seq && g_rt.seq_count > 0) {
                for (uint32_t i = 0; i < g_rt.seq_count; i++) {
                    void* layer = nullptr;
                    __try { layer = *(void**)((uintptr_t)g_rt.seq + OFF_ARR_DATA + 8 * i); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                    if (layer) DumpLayerPhases(layer, i);
                }
            }
        }
        // periodic summary
        if (g_rt.frame - g_rt.last_summary >= (uint64_t)g_cfg.log_every_frames) {
            g_rt.last_summary = g_rt.frame;
            Log("[summary] frame=%llu state=0x%llX action=%p vtbl=0x%llX seq_layers=%u",
                (unsigned long long)g_rt.frame,
                (unsigned long long)s, g_rt.last_action,
                (unsigned long long)g_rt.last_vtable, g_rt.seq_count);
        }
    }
    if (o_CtrlUp) {
        __try { o_CtrlUp(self, mask); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// selUpdate(this) - passthrough only (no state we need here)
static void hk_SelUp(void* self) {
    if (o_SelUp) {
        __try { o_SelUp(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ─────────────────────────────────────────────────────────────
// Install
// ─────────────────────────────────────────────────────────────
struct HookInst {
    uintptr_t rva;
    void* fn;
    void** orig;
    const char* name;
    bool ok;
};
static HookInst g_hooks[] = {
    { RVA_CB_GETACTIONSTATE, (void*)hk_GetState,      (void**)&o_GetState,      "CharacterBase.getActionState",          false },
    { RVA_CB_GETBASEACTION,  (void*)hk_GetBaseAction, (void**)&o_GetBaseAction, "CharacterBase.get_BaseCurrentAction",   false },
    { RVA_PCE_GETCANCELSEQ,  (void*)hk_GetSeq,        (void**)&o_GetSeq,        "PCE.getCancelMotionSeq",                false },
    { RVA_PCC_UPDATE,        (void*)hk_CtrlUp,        (void**)&o_CtrlUp,        "PCC.update",                            false },
    { RVA_PAS_UPDATE,        (void*)hk_SelUp,         (void**)&o_SelUp,         "PAS.update",                            false },
};
static constexpr size_t kHookCount = sizeof(g_hooks) / sizeof(g_hooks[0]);

static int InstallAll() {
    if (MH_Initialize() != MH_OK) { Log("[-] MH_Initialize failed"); return 0; }
    int ok = 0;
    for (auto& h : g_hooks) {
        void* t = (void*)(kBase + h.rva);
        MH_STATUS s1 = MH_CreateHook(t, h.fn, h.orig);
        MH_STATUS s2 = (s1 == MH_OK) ? MH_EnableHook(t) : s1;
        if (s2 != MH_OK) {
            Log("[-] %s failed (%d)", h.name, (int)s2);
        } else {
            Log("[+] %s hooked @ 0x%llX", h.name, (unsigned long long)h.rva);
            h.ok = true;
            ok++;
        }
    }
    return ok;
}

static void UninstallAll() {
    for (auto& h : g_hooks) if (h.ok) MH_DisableHook((void*)(kBase + h.rva));
    MH_Uninitialize();
    Log("[i] hooks removed");
}

// ─────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────
static DWORD WINAPI MainThread(LPVOID) {
    LoadConfig(true);
    for (int i = 0; i < 240; i++) {
        HMODULE mod = GetModuleHandleA("OnimushaWotS.exe");
        if (!mod) { Sleep(500); continue; }
        auto* dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = (IMAGE_NT_HEADERS*)((uintptr_t)mod + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kTimestamp ||
            nt->OptionalHeader.SizeOfImage != kSizeOfImage) {
            Log("[-] unsupported build");
            return 0;
        }
        Log("[+] build verified: %p", mod);
        int n = InstallAll();
        if (n == 0) { Log("[-] no hooks"); return 0; }
        Log("[+] DUMP READY (%d hooks).", n);
        Log("[+] Play: light attack x3, hard attack, dodge, guard, parry.");
        return 0;
    }
    Log("[-] game not found");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        char p[MAX_PATH];
        GetModuleFileNameA(h, p, MAX_PATH);
        g_log_path = p;
        size_t s = g_log_path.find_last_of("\\/");
        g_log_path = (s == std::string::npos ? "" : g_log_path.substr(0, s + 1)) + "OnimushaMod_log.txt";
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        UninstallAll();
    }
    return TRUE;
}
